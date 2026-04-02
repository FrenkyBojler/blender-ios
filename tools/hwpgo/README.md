# Hardware Profile Guided Optimization in Blender

## Requirements
- Python 3
- Clang compiler (MSVC is not supported)
- LLVM tools, per default shipped with LLVM: `llvm-profgen`, `llvm-profdata`, `llvm-readobj`
- Intel [VTune](https://www.intel.com/content/www/us/en/developer/tools/oneapi/vtune-profiler-download.html) (provides the `sep` command line collector)
- A representative workload, for example scenes from the [Blender Demo Files](https://www.blender.org/download/demo-files/)
- A CPU with the `BR_INST_RETIRED.NEAR_TAKEN:pdir` event and Last Branch Records (Intel Skylake and above)

## Workflow overview
1. Compile with debug information (`HWPGO_MODE=GENERATE`)
2. Collect performance data with VTune sep
3. Convert performance data to an LLVM profile
4. Compile the optimized build with that profile (`HWPGO_MODE=USE`)

## Scripted workflow for the blender benchmark

### 1. Compile and install with HWPGO debug symbols
Use your normal cmake configuration and add the HWPGO flags:
```
cmake -DWITH_HWPGO=ON -DHWPGO_MODE=GENERATE <your other flags>
```
This adds `-gdwarf -gsplit-dwarf -funique-internal-linkage-names -fdebug-info-for-profiling` and the matching linker flags.
Install the resulting build to a separate folder, for example `blender_generate`.

### 2. Collect performance data
a) Open an administrator command prompt

b) Load the VTune environment:
```
"C:\Program Files (x86)\Intel\oneAPI\vtune\latest\sep_vars.cmd"
```

c) Run the collection script. It takes a workload definition in JSON format (see below) and drives `sep` for each scene:
```
python collect_hwpgo_performance_data.py ^
    --blender C:\blender_generate\blender.exe ^
    --workload workload.json ^
    --event-collection C:\hwpgo_collections ^
    --llvm C:\llvm\bin
```

The workload JSON defines blender cmd calls with a set of flags:
```json
{
    "name": "benchmark",
    "workloads": [
        {
            "name": "classroom",
            "args": ["-b", "scenes/cyclesx_classroom.blend", "-E", "CYCLES", "-f", "1"]
        },
        {
            "name": "junkshop",
            "args": ["-b", "scenes/cyclesx_junkshop.blend", "-E", "CYCLES", "-f", "1"]
        }
        ...
    ]
}
```
Each entry runs blender with the given arguments while `sep` records branch events.
In this example we put the classroom and junkshop scenes into tools/hwpgo/scenes.
Ensure to adjust the paths and scene names to the demo scenes you downloaded.

### 3. Convert performance data to LLVM profile
Use the profile creation script to convert the sep results from step 2 into a merged LLVM profile:
```
python create_hwpgo_profile.py ^
    --blender C:\blender_generate\blender.exe ^
    --event-collection C:\hwpgo_collections ^
    --profile C:\hwpgo_profile\benchmark.prof ^
    --llvm C:\llvm\bin
```
This runs `llvm-profgen` for every binary/collection combination and then merges everything with `llvm-profdata`.

### 4. Compile the optimized build
Configure cmake with the profile from step 3:
```
cmake -DWITH_HWPGO=ON -DHWPGO_MODE=USE -DHWPGO_PROFILE=C:\hwpgo_profile\benchmark.prof <your other flags>
```
This adds `-fprofile-sample-use=<profile>` and enables the sample-profile optimization remarks.

## Manual workflow
Sometimes it is not possible to use the automated collection scripts, in this case you can use the toolchain manually to create a profile.
Follow the scripted workflow up to step 2b, then continue below.

### 2c) Collect with sep directly
```
sep -start -out performance_data.tb7 -ec "BR_INST_RETIRED.NEAR_TAKEN:SA=1000003:pdir:lbr:USR=YES" -lbr no_filter:usr -perf-script ip,brstack -app BLENDER_COMMAND
```
For example:
```
sep -start -out collection_classroom.tb7 ^
    -ec "BR_INST_RETIRED.NEAR_TAKEN:SA=1000003:pdir:lbr:USR=YES" ^
    -lbr no_filter:usr -perf-script ip,brstack ^
    -app "blender.exe -b scenes/cyclesx_classroom.blend -E CYCLES -f 1"
```
The command will terminate once your application has finished and the collection is done.
As output you get a file `collection_classroom.tb7` which after the collection finished is converted into a processable `collection_classroom.perf.data.script`.
You can repeat this step for as many workloads as you like.

### 3) Convert to LLVM profile
Generate a profile for each binary that has debug symbols:
```
llvm-profgen.exe --binary blender.exe --perfscript collection_classroom.perf.data.script --output collection_classroom.blender.exe.prof
```
Repeat for every binary/collection combination, then merge all profiles:
```
llvm-profdata.exe merge --text --sample --output benchmark.prof collection_classroom.blender.exe.prof collection_junkshop.blender.exe.prof ...
```
Use the resulting `benchmark.prof` in step 4 of the scripted workflow.

## Files
| File | Description |
|------|-------------|
| `collect_hwpgo_performance_data.py` | Drives sep collections for all scenes defined in a workload JSON |
| `create_hwpgo_profile.py` | Converts sep output into a merged LLVM sample profile |
| `hwpgo_utils.py` | Shared helpers (binary discovery, symbol validation, LLVM tool checks) |
| `benchmark_profile.prof` | Example/reference profile generated from the Blender Open Data benchmark scenes |
