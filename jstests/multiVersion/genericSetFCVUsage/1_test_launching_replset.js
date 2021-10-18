//
// Tests launching multi-version ReplSetTest replica sets
//
// @tags: [disabled_due_to_server_60490]
//

load('./jstests/multiVersion/libs/verify_versions.js');

for (let version of ["last-lts", "last-continuous", "latest"]) {
    jsTestLog("Testing single version: " + version);

    // Set up a single-version replica set
    var rst = new ReplSetTest({nodes: 2});

    rst.startSet({binVersion: version});

    var nodes = rst.nodes;

    // Make sure the started versions are actually the correct versions
    for (var j = 0; j < nodes.length; j++)
        assert.binVersion(nodes[j], version);

    rst.stopSet();
}

for (let versions of [["last-lts", "latest"], ["last-continuous", "latest"]]) {
    jsTestLog("Testing mixed versions: " + tojson(versions));

    // Set up a multi-version replica set
    var rst = new ReplSetTest({nodes: 2});

    rst.startSet({binVersion: versions});

    var nodes = rst.nodes;

    // Make sure we have hosts of all the different versions
    var versionsFound = [];
    for (var j = 0; j < nodes.length; j++)
        versionsFound.push(nodes[j].getBinVersion());

    assert.allBinVersions(versions, versionsFound);

    rst.stopSet();
}

for (let versions of [["last-lts", "last-continuous"], ["last-continuous", "last-lts"]]) {
    jsTestLog("Testing mixed versions: " + tojson(versions));

    try {
        var rst = new ReplSetTest({nodes: 2});
    } catch (e) {
        if (e instanceof Error) {
            assert.includes(
                e.message,
                "Can only specify one of 'last-lts' and 'last-continuous' in binVersion, not both.");
            continue;
        } else {
            throw e;
        }
    }
    assert(
        MongoRunner.areBinVersionsTheSame("last-continuous", "last-lts"),
        "Should have thrown error in creating ReplSetTest because can only specify one of 'last-lts' and 'last-continuous' in binVersion, not both.");

    rst.startSet({binVersion: versions});

    var nodes = rst.nodes;

    // Make sure we have hosts of all the different versions
    var versionsFound = [];
    for (var j = 0; j < nodes.length; j++)
        versionsFound.push(nodes[j].getBinVersion());

    assert.allBinVersions(versions, versionsFound);

    rst.stopSet();
}

jsTestLog("Done!");

//
// End
//
