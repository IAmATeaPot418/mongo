import subprocess
import sys
import json
import os
import shutil
import argparse

parser = argparse.ArgumentParser(description='Ninja Bazel builder.')

parser.add_argument('--ninja-file', type=str, help="The ninja file in use", default="build.ninja")
parser.add_argument('--verbose', action='store_true', help="Turn on verbose mode")
parser.add_argument('--integration-debug', action='store_true',
                    help="Turn on extra debug output about the ninja-bazel integration")

args = parser.parse_args()

# This corresponds to BAZEL_INTEGRATION_DEBUG=1 from SCons command line
if args.integration_debug:

    def print_debug(msg):
        print("[BAZEL_INTEGRATION_DEBUG] " + msg)
else:

    def print_debug(msg):
        pass


# our ninja python module intercepts the command lines and
# prints out the targets everytime ninja is executed
ninja_command_line_targets = []
try:
    ninja_last_cmd_file = '.ninja_last_command_line_targets.txt'
    with open(ninja_last_cmd_file) as f:
        ninja_command_line_targets = [target.strip() for target in f.readlines() if target.strip()]
    os.unlink(ninja_last_cmd_file)
except OSError as exc:
    print(f"""
Failed to open {ninja_last_cmd_file}, this is expected to be generated on ninja execution by the mongo-ninja-python module.
          
Make sure you are in the recommended virtualenv: 
https://github.com/10gen/mongo/blob/master/docs/building.md#python-prerequisites
          
The command `hash -r && which ninja` should reference a ninja file located within the virtualenv. 
If this is not the case, the virtualenv may have become corrupted and you will need to delete it
and create a new one from scratch based on the commands in the doc link above.
""")
    raise exc
print_debug(f"NINJA COMMAND LINE TARGETS:{os.linesep}{os.linesep.join(ninja_command_line_targets)}")

# Our ninja generation process generates all the build info related to
# the specific ninja file
ninja_build_info = dict()
try:
    bazel_info_file = '.bazel_info_for_ninja.txt'
    with open(bazel_info_file) as f:
        ninja_build_info = json.load(f)
except OSError as exc:
    print(
        f"Failed to open {bazel_info_file}, this is expected to be generated by scons during ninja generation."
    )
    raise exc

# flip the targets map for optimized use later
bazel_out_to_bazel_target = dict()
for bazel_t in ninja_build_info['targets'].values():
    bazel_out_to_bazel_target[bazel_t['bazel_output']] = bazel_t['bazel_target']

# run ninja and get the deps from the passed command line targets so we can check if any deps are bazel targets
ninja_inputs_cmd = ['ninja', '-f', args.ninja_file, '-t', 'inputs'] + ninja_command_line_targets
print_debug(f"NINJA GET INPUTS CMD: {' '.join(ninja_inputs_cmd)}")

ninja_proc = subprocess.run(ninja_inputs_cmd, capture_output=True, text=True, check=True)
deps = [os.path.abspath(dep).replace("\\", '/') for dep in ninja_proc.stdout.split("\n") if dep]
print_debug(f"COMMAND LINE DEPS:{os.linesep}{os.linesep.join(deps)}")
os.unlink(ninja_last_cmd_file)

# isolate just the raw output files for the list intersection
bazel_outputs = [bazel_t['bazel_output'] for bazel_t in ninja_build_info['targets'].values()]
print_debug(f"BAZEL OUTPUTS:{os.linesep}{os.linesep.join(bazel_outputs)}")

# now out of possible bazel outputs find which are deps of the requested command line targets
outputs_to_build = list(set(deps).intersection(bazel_outputs))
print_debug(f"BAZEL OUTPUTS TO BUILD: {outputs_to_build}")

# convert from outputs (raw files) to bazel targets (bazel labels i.e //src/db/mongo:target)
targets_to_build = [bazel_out_to_bazel_target[out] for out in outputs_to_build]

if not targets_to_build:
    print("Did not find any bazel targets to build, bazel should not have been invoked.")
    print(f"correspondig raw output files to build: {outputs_to_build}")
    print(f"command line targets: {ninja_command_line_targets}")
    print(f"input target deps: {deps}")
    sys.exit(1)

# ninja will automatically create directories for any outputs, but in this case
# bazel will be creating a symlink for the bazel-out dir to its cache. We don't want
# ninja to interfere so delete the dir if it was not a link (made by bazel)
if sys.platform == "win32":
    if os.path.exists("bazel-out"):
        try:
            os.readlink("bazel-out")
        except OSError:
            shutil.rmtree("bazel-out")

else:
    if not os.path.islink("bazel-out"):
        shutil.rmtree("bazel-out")

# now we are ready to build all bazel buildable files
print_debug(f"BAZEL TARGETS TO BUILD:{os.linesep}{os.linesep.join(targets_to_build)}")
if args.verbose:
    print(f"{' '.join(ninja_build_info['bazel_cmd'] + targets_to_build)}")
    extra_args = []
else:
    extra_args = ["--ui_event_filters=-info,-debug,-warning,-stderr,-stdout"]
bazel_proc = subprocess.run(ninja_build_info['bazel_cmd'] + extra_args + targets_to_build,
                            check=True)
