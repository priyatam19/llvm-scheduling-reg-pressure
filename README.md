# To start the container
```
docker run -it --name llvm-dev -v "$PWD":/work -w /work priyatam19/compiler-opts:llvm21 /bin/bash
docker start -ai llvm-dev
docker exec -it llvm-dev /bin/bash
```

# Overview
```
Three configurations are evaluated, local scheduling, global with register pressure, and global without. The third one exists specifically to isolate how much register pressure awareness contributes to the final result. Two tools are used for evaluation. LLVM MCA analyzes the generated RISC-V IR and gives static estimates of instruction count, cycle count, throughput and register spills. GEM5 is a RISC-V simulator that runs the actual compiled binaries and gives real execution data. Together they give both a static view and a simulated runtime view of the scheduling impact.
```

# Downloading Mibench
```
curl -L https://github.com/embecosm/mibench/archive/refs/heads/master.zip -o /tmp/mibench.zip
apt-get update && apt-get install unzip -y
unzip /tmp/mibench.zip -d /tmp/
cp -r /tmp/mibench-master benchmarks/

```

# To compile, run and get results for all the pass 
## Run this commands inside the project directory (/work)
```

make

apt-get install lld -y
apt-get install binutils-riscv64-linux-gnu -y
apt-get install gcc-riscv64-linux-gnu -y

chmod +x benchmark.sh

./benchmark.sh 

chmod +x mibench.sh

./mibench.sh 

```

# To run the .obj files in GEM5 RISC simulator
## You should have GEM5 in the container
## Else build a container with this image and run the following scripts 
## from /work directory
```

docker pull rseac/gem5:v24-0

docker run -it --rm -v $(pwd):/work rseac/gem5:v24-0 bash


(run this before ./benchmark.sh)

chmod +x gem5_benchmark.sh 
./gem5_benchmark.sh 

(run this before ./mibench.sh )

chmod +x gem5_mibench.sh
./gem5_mibench.sh 

```