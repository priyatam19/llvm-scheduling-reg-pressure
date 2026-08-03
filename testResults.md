## Overview
```
Three configurations are evaluated, local scheduling, global with register pressure, and global without. The third one exists specifically to isolate how much register pressure awareness contributes to the final result. Two tools are used for evaluation. LLVM MCA analyzes the generated RISC-V IR and gives static estimates of instruction count, cycle count, throughput and register spills. GEM5 is a RISC-V simulator that runs the actual compiled binaries and gives real execution data. Together they give both a static view and a simulated runtime view of the scheduling impact.

```

### Benchmark Program

```
root@1e4a1cf83886:/work# ./benchmark.sh
localSchedulerPass running on: @benchmark

localSchedulerPass running on: @main

globalSchedulerPass running on: @benchmark | Register Pressure: enabled

globalSchedulerPass running on: @main | Register Pressure: enabled

globalSchedulerPass running on: @benchmark | Register Pressure: disabled

globalSchedulerPass running on: @main | Register Pressure: disabled

=== To verify correctness (baseline, local, global, global_no_pressure) ===
Exit: 64
Exit: 64
Exit: 64
Exit: 64


=== These following outputs are from llvm-mca ===
=== Please run gem5_benchmark.sh to get results from simulation ===


=== Orginal Code ===

Instructions:      12400
Total Cycles:      24601
Total uOps:        12500
Dispatch Width:    1
uOps Per Cycle:    0.51
IPC:               0.50
Block RThroughput: 125.0

Register Spills:
11


=== Local Scheduler (Baseline) ===

Instructions:      11800
Total Cycles:      24001
Total uOps:        11900
Dispatch Width:    1
uOps Per Cycle:    0.50
IPC:               0.49
Block RThroughput: 119.0

Register Spills:
8


=== Global Scheduler (with Register Pressure) ===

Instructions:      11700
Total Cycles:      23801
Total uOps:        11800
Dispatch Width:    1
uOps Per Cycle:    0.50
IPC:               0.49
Block RThroughput: 118.0

Register Spills:
8


=== Global Scheduler (without Register Pressure) ===

Instructions:      12300
Total Cycles:      24401
Total uOps:        12400
Dispatch Width:    1
uOps Per Cycle:    0.51
IPC:               0.50
Block RThroughput: 124.0

Register Spills:
11

root@1e4a1cf83886:/work# 

```

```
root@3b1b58e7a0e5:/work# ./gem5_benchmark.sh 

Running: build/original_benchmark
Cycles (Number of cpu cycles simulated): 79075703
Instructions (Number of instructions committed (thread level)): 71006423
IPC (IPC: instructions per cycle (core level)): 0.897955
SimSeconds (Number of seconds simulated): 0.039538

Running: build/run_local
Cycles (Number of cpu cycles simulated): 72075981
Instructions (Number of instructions committed (thread level)): 65006382
IPC (IPC: instructions per cycle (core level)): 0.901915
SimSeconds (Number of seconds simulated): 0.036038

Running: build/run_global
Cycles (Number of cpu cycles simulated): 64076061
Instructions (Number of instructions committed (thread level)): 64006412
IPC (IPC: instructions per cycle (core level)): 0.998913
SimSeconds (Number of seconds simulated): 0.032038

Running: build/run_global_no_pressure
Cycles (Number of cpu cycles simulated): 78075423
Instructions (Number of instructions committed (thread level)): 70006396
IPC (IPC: instructions per cycle (core level)): 0.896651
SimSeconds (Number of seconds simulated): 0.039038
root@3b1b58e7a0e5:/work# ./gem5_mibench.sh 
```

### Complete Cold Path Scenario

```
root@1e4a1cf83886:/work# ./benchmark.sh
localSchedulerPass running on: @benchmark

localSchedulerPass running on: @main

globalSchedulerPass running on: @benchmark | Register Pressure: enabled

globalSchedulerPass running on: @main | Register Pressure: enabled

globalSchedulerPass running on: @benchmark | Register Pressure: disabled

globalSchedulerPass running on: @main | Register Pressure: disabled

=== To verify correctness (baseline, local, global, global_no_pressure) ===
Exit: 64
Exit: 64
Exit: 64
Exit: 64


=== These following outputs are from llvm-mca ===
=== Please run gem5_benchmark.sh to get results from simulation ===


=== Orginal Code ===

Instructions:      12400
Total Cycles:      24601
Total uOps:        12500
Dispatch Width:    1
uOps Per Cycle:    0.51
IPC:               0.50
Block RThroughput: 125.0

Register Spills:
11


=== Local Scheduler (Baseline) ===

Instructions:      11800
Total Cycles:      24001
Total uOps:        11900
Dispatch Width:    1
uOps Per Cycle:    0.50
IPC:               0.49
Block RThroughput: 119.0

Register Spills:
8


=== Global Scheduler (with Register Pressure) ===

Instructions:      12500
Total Cycles:      24601
Total uOps:        12600
Dispatch Width:    1
uOps Per Cycle:    0.51
IPC:               0.51
Block RThroughput: 126.0

Register Spills:
12


=== Global Scheduler (without Register Pressure) ===

Instructions:      12500
Total Cycles:      24601
Total uOps:        12600
Dispatch Width:    1
uOps Per Cycle:    0.51
IPC:               0.51
Block RThroughput: 126.0

Register Spills:
12

root@1e4a1cf83886:/work# 
```

```
root@3b1b58e7a0e5:/work# ./gem5_benchmark.sh 

Running: build/original_benchmark
Cycles (Number of cpu cycles simulated): 30076211
Instructions (Number of instructions committed (thread level)): 26006423
IPC (IPC: instructions per cycle (core level)): 0.864684
SimSeconds (Number of seconds simulated): 0.015038

Running: build/run_local
Cycles (Number of cpu cycles simulated): 31075485
Instructions (Number of instructions committed (thread level)): 26006382
IPC (IPC: instructions per cycle (core level)): 0.836878
SimSeconds (Number of seconds simulated): 0.015538

Running: build/run_global
Cycles (Number of cpu cycles simulated): 31076231
Instructions (Number of instructions committed (thread level)): 25006412
IPC (IPC: instructions per cycle (core level)): 0.804680
SimSeconds (Number of seconds simulated): 0.015538

Running: build/run_global_no_pressure
Cycles (Number of cpu cycles simulated): 31076555
Instructions (Number of instructions committed (thread level)): 25006396
IPC (IPC: instructions per cycle (core level)): 0.804671
SimSeconds (Number of seconds simulated): 0.015538
root@3b1b58e7a0e5:/work# 
```

### Mibench
```
root@1e4a1cf83886:/work# ./mibench.sh 
f1f60574032428e7 b758c7f0666834bc 51c1213c7bc95b05 6ac258e3b3ea96d0 62f70c93ef147fc8
localSchedulerPass running on: @sha_init

localSchedulerPass running on: @sha_update

localSchedulerPass running on: @byte_reverse

localSchedulerPass running on: @sha_transform

localSchedulerPass running on: @sha_final

localSchedulerPass running on: @sha_stream

localSchedulerPass running on: @sha_print

localSchedulerPass running on: @main

globalSchedulerPass running on: @sha_init | Register Pressure: enabled

globalSchedulerPass running on: @sha_update | Register Pressure: enabled

globalSchedulerPass running on: @byte_reverse | Register Pressure: enabled

globalSchedulerPass running on: @sha_transform | Register Pressure: enabled

globalSchedulerPass running on: @sha_final | Register Pressure: enabled

globalSchedulerPass running on: @sha_stream | Register Pressure: enabled

globalSchedulerPass running on: @sha_print | Register Pressure: enabled

globalSchedulerPass running on: @main | Register Pressure: enabled

globalSchedulerPass running on: @sha_init | Register Pressure: disabled

globalSchedulerPass running on: @sha_update | Register Pressure: disabled

globalSchedulerPass running on: @byte_reverse | Register Pressure: disabled

globalSchedulerPass running on: @sha_transform | Register Pressure: disabled

globalSchedulerPass running on: @sha_final | Register Pressure: disabled

globalSchedulerPass running on: @sha_stream | Register Pressure: disabled

globalSchedulerPass running on: @sha_print | Register Pressure: disabled

globalSchedulerPass running on: @main | Register Pressure: disabled


=== These following outputs are from llvm-mca ===
=== Please run gem5_benchmark.sh to get results from simulation ===


=== Local Scheduler (Baseline) ===

Instructions:      44100
Total Cycles:      269501
Total uOps:        46400
Dispatch Width:    1
uOps Per Cycle:    0.17
IPC:               0.16
Block RThroughput: 464.0

Register Spills:
33


=== Global Scheduler (with Register Pressure) ===

Instructions:      44200
Total Cycles:      269501
Total uOps:        46500
Dispatch Width:    1
uOps Per Cycle:    0.17
IPC:               0.16
Block RThroughput: 465.0

Register Spills:
33


=== Global Scheduler (without Register Pressure) ===

Instructions:      45000
Total Cycles:      270301
Total uOps:        47300
Dispatch Width:    1
uOps Per Cycle:    0.17
IPC:               0.17
Block RThroughput: 473.0

Register Spills:
35

root@1e4a1cf83886:/work# 
```