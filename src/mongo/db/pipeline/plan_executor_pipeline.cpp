/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/plan_executor_pipeline.h"

#include "mongo/db/pipeline/change_stream_topology_change_info.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/plan_explainer_pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/speculative_majority_read_info.h"

namespace mongo {

PlanExecutorPipeline::PlanExecutorPipeline(boost::intrusive_ptr<ExpressionContext> expCtx,
                                           std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                                           ResumableScanType resumableScanType)
    : _expCtx(std::move(expCtx)),
      _pipeline(std::move(pipeline)),
      _planExplainer{_pipeline.get()},
      _resumableScanType{resumableScanType} {
    // Pipeline plan executors must always have an ExpressionContext.
    invariant(_expCtx);

    // The caller is responsible for disposing this plan executor before deleting it, which will in
    // turn dispose the underlying pipeline. Therefore, there is no need to dispose the pipeline
    // again when it is destroyed.
    _pipeline.get_deleter().dismissDisposal();

    if (ResumableScanType::kNone != resumableScanType) {
        // For a resumable scan, set the initial _latestOplogTimestamp and _postBatchResumeToken.
        _initializeResumableScanState();
    }
}

PlanExecutor::ExecState PlanExecutorPipeline::getNext(BSONObj* objOut, RecordId* recordIdOut) {
    // The pipeline-based execution engine does not track the record ids associated with documents,
    // so it is an error for the caller to ask for one. For the same reason, we expect the caller to
    // provide a non-null BSONObj pointer for 'objOut'.
    invariant(!recordIdOut);
    invariant(objOut);

    if (!_stash.empty()) {
        *objOut = std::move(_stash.front());
        _stash.pop();
        _planExplainer.incrementNReturned();
        return PlanExecutor::ADVANCED;
    }

    Document docOut;
    auto execState = getNextDocument(&docOut, nullptr);
    if (execState == PlanExecutor::ADVANCED) {
        // Include metadata if the output will be consumed by a merging node.
        *objOut = _expCtx->needsMerge ? docOut.toBsonWithMetaData() : docOut.toBson();
    }
    return execState;
}

PlanExecutor::ExecState PlanExecutorPipeline::getNextDocument(Document* docOut,
                                                              RecordId* recordIdOut) {
    // The pipeline-based execution engine does not track the record ids associated with documents,
    // so it is an error for the caller to ask for one. For the same reason, we expect the caller to
    // provide a non-null pointer for 'docOut'.
    invariant(!recordIdOut);
    invariant(docOut);

    // Callers which use 'enqueue()' are not allowed to use 'getNextDocument()', and must instead
    // use 'getNext()'.
    invariant(_stash.empty());

    if (auto next = _getNext()) {
        *docOut = std::move(*next);
        _planExplainer.incrementNReturned();
        return PlanExecutor::ADVANCED;
    }

    return PlanExecutor::IS_EOF;
}

bool PlanExecutorPipeline::isEOF() {
    if (!_stash.empty()) {
        return false;
    }

    return _pipelineIsEof;
}

boost::optional<Document> PlanExecutorPipeline::_getNext() {
    auto nextDoc = _tryGetNext();
    if (!nextDoc) {
        _pipelineIsEof = true;
    }

    if (ResumableScanType::kNone != _resumableScanType) {
        _updateResumableScanState(nextDoc);
    }
    return nextDoc;
}

boost::optional<Document> PlanExecutorPipeline::_tryGetNext() try {
    return _pipeline->getNext();
} catch (const ExceptionFor<ErrorCodes::ChangeStreamTopologyChange>& ex) {
    // This exception contains the next document to be returned by the pipeline.
    const auto extraInfo = ex.extraInfo<ChangeStreamTopologyChangeInfo>();
    tassert(5669600, "Missing ChangeStreamTopologyChangeInfo on exception", extraInfo);
    return Document::fromBsonWithMetaData(extraInfo->getTopologyChangeEvent());
}

void PlanExecutorPipeline::_updateResumableScanState(const boost::optional<Document>& document) {
    switch (_resumableScanType) {
        case ResumableScanType::kChangeStream:
            _performChangeStreamsAccounting(document);
            break;
        case ResumableScanType::kOplogScan:
            _performResumableOplogScanAccounting();
            break;
        case ResumableScanType::kNone:
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(5353402);
    }
}

void PlanExecutorPipeline::_performChangeStreamsAccounting(const boost::optional<Document>& doc) {
    tassert(5353405,
            "expected _resumableScanType == kChangeStream",
            ResumableScanType::kChangeStream == _resumableScanType);
    if (doc) {
        // While we have more results to return, we track both the timestamp and the resume token of
        // the latest event observed in the oplog, the latter via its sort key metadata field.
        _validateChangeStreamsResumeToken(*doc);
        _latestOplogTimestamp = PipelineD::getLatestOplogTimestamp(_pipeline.get());
        _postBatchResumeToken = doc->metadata().getSortKey().getDocument().toBson();
        _setSpeculativeReadTimestamp();
    } else {
        // We ran out of results to return. Check whether the oplog cursor has moved forward since
        // the last recorded timestamp. Because we advance _latestOplogTimestamp for every event we
        // return, if the new time is higher than the last then we are guaranteed not to have
        // already returned any events at this timestamp. We can set _postBatchResumeToken to a new
        // high-water-mark token at the current clusterTime.
        auto highWaterMark = PipelineD::getLatestOplogTimestamp(_pipeline.get());
        if (highWaterMark > _latestOplogTimestamp) {
            auto token = ResumeToken::makeHighWaterMarkToken(highWaterMark);
            _postBatchResumeToken = token.toDocument().toBson();
            _latestOplogTimestamp = highWaterMark;
            _setSpeculativeReadTimestamp();
        }
    }
}

void PlanExecutorPipeline::_validateChangeStreamsResumeToken(const Document& event) const {
    // If we are producing output to be merged on mongoS, then no stages can have modified the _id.
    if (_expCtx->needsMerge) {
        return;
    }

    // Confirm that the document _id field matches the original resume token in the sort key field.
    auto eventBSON = event.toBson();
    auto resumeToken = event.metadata().getSortKey();
    auto idField = eventBSON.getObjectField("_id");
    invariant(!resumeToken.missing());
    uassert(ErrorCodes::ChangeStreamFatalError,
            str::stream() << "Encountered an event whose _id field, which contains the resume "
                             "token, was modified by the pipeline. Modifying the _id field of an "
                             "event makes it impossible to resume the stream from that point. Only "
                             "transformations that retain the unmodified _id field are allowed. "
                             "Expected: "
                          << BSON("_id" << resumeToken) << " but found: "
                          << (eventBSON["_id"] ? BSON("_id" << eventBSON["_id"]) : BSONObj()),
            (resumeToken.getType() == BSONType::Object) &&
                idField.binaryEqual(resumeToken.getDocument().toBson()));
}

void PlanExecutorPipeline::_performResumableOplogScanAccounting() {
    tassert(5353404,
            "expected _resumableScanType == kOplogScan",
            ResumableScanType::kOplogScan == _resumableScanType);

    // Update values of latest oplog timestamp and postBatchResumeToken.
    _latestOplogTimestamp = PipelineD::getLatestOplogTimestamp(_pipeline.get());
    _postBatchResumeToken = PipelineD::getPostBatchResumeToken(_pipeline.get());
    _setSpeculativeReadTimestamp();
}

void PlanExecutorPipeline::_setSpeculativeReadTimestamp() {
    repl::SpeculativeMajorityReadInfo& speculativeMajorityReadInfo =
        repl::SpeculativeMajorityReadInfo::get(_expCtx->opCtx);
    if (speculativeMajorityReadInfo.isSpeculativeRead() && !_latestOplogTimestamp.isNull()) {
        speculativeMajorityReadInfo.setSpeculativeReadTimestampForward(_latestOplogTimestamp);
    }
}

void PlanExecutorPipeline::_initializeResumableScanState() {
    switch (_resumableScanType) {
        case ResumableScanType::kChangeStream:
            // Set _postBatchResumeToken to the initial PBRT that was added to the expression
            // context during pipeline construction, and use it to obtain the starting time for
            // _latestOplogTimestamp.
            tassert(5353403,
                    "expected initialPostBatchResumeToken to be not empty",
                    !_expCtx->initialPostBatchResumeToken.isEmpty());
            _postBatchResumeToken = _expCtx->initialPostBatchResumeToken.getOwned();
            _latestOplogTimestamp = ResumeToken::parse(_postBatchResumeToken).getData().clusterTime;
            break;
        case ResumableScanType::kOplogScan:
            // Initialize the oplog timestamp and postBatchResumeToken here in case the request has
            // batchSize 0, in which case the PBRT of the first batch would be empty.
            _performResumableOplogScanAccounting();
            break;
        case ResumableScanType::kNone:
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(5353401);
    }
}

void PlanExecutorPipeline::markAsKilled(Status killStatus) {
    invariant(!killStatus.isOK());
    // If killed multiple times, only retain the first status.
    if (_killStatus.isOK()) {
        _killStatus = killStatus;
    }
}

}  // namespace mongo
