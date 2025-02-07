#! /usr/bin/env bash

# This script uses RocksDB + YCSB that simulte real-world database usage as L-tenants

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

EVAL_DIR=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
YCSB_DIR=$EVAL_DIR/ycsb
BASE_OUT_DIR=$EVAL_DIR/results-rocksdb-ycsb-mt-sh

if [[ ! -d "$YCSB_DIR" ]]; then
	echo "No directory of YCSB benchmark under $EVAL_DIR"
	exit 1
fi

## Arguments for RocksDB
db_dir=''   # directory fo the RocksDB database directory
db_opts=''  # the rocksdb opts for YCSB
nr_bg_jobs=''   # background jobs of RocksDB

## Arguments for YCSB
ycsb_workload=''   # the type of YCSB workload
ycsb_workload_opts=''

## Arguments for FIO
nr_fio_tenant=''	# number of FIO T-tenants
rw_fio_tenant=''	# type of FIO T-tenants
bs_fio_tenant=''
depth_fio_tenant=''
fio_dir=''

## Environment arguments
cpus=''
sys=''	# evaluated system
res_dir=''
nvme_dev=''

## Predefined variable
fio_file_size=32G

while (( "$#" )); do
	case "$1" in
	  --db_dir)
		db_dir="$2"
		shift 2
		;;
	  --db_opts)
		db_opts="$2"
		shift 2
		;;
      --bg_jobs)
        nr_bg_jobs="$2"
        shift 2
        ;;
	  --workload)
		ycsb_workload="$2"
		shift 2
		;;
	  --nr_fio)
		IFS=',' read -a nr_fio_tenant <<< "$2"
	  	shift 2
	  	;;
	  --rw_fio)
		rw_fio_tenant="$2"
		shift 2
		;;
	  --bs_fio)
		bs_fio_tenant="$2"
		shift 2
		;;
	  --depth_fio)
		depth_fio_tenant="$2"
		shift 2
		;;
	  --fio_dir)
	  	fio_dir="$2"
		shift 2
		;;
	  --cpus)
		cpus="$2"
		shift 2
		;;
	  --sys)
		sys="$2"
		shift 2
		;;
	  --nvme_dev)
		nvme_dev="$2"
		shift 2
		;;
	  --res_dir)
		res_dir="$2"
		shift 2
		;;
	  *)
		usage
		exit 1
	esac
done

# The YCSB opts of the given workload type
ycsb_workload_opts=$EVAL_DIR/sh-ycsb-$ycsb_workload.opts

if [[ -z $sys || -z $res_dir ]]; then
	echo "Missing specifying the evaluated system or results directory name...Exiting...."
	exit 1
fi

OUT_DIR=$BASE_OUT_DIR/$sys/$res_dir
if [[ ! -d "$OUT_DIR" ]]; then
	echo "The output directory for multi-tenant FIO is not present, creating $OUT_DIR"
	mkdir -p $OUT_DIR
fi

declare -a allowed_cpus
nr_tot_cpus=''
## get total cpus
if [[ $cpus == *"-"* ]];then
	IFS='-' read -ra RANGE <<< "$cpus"
	cpu_st=${RANGE[0]}
	cpu_ed=${RANGE[1]}
	nr_tot_cpus=$((cpu_ed - cpu_st + 1))
	for cpu in $(seq $cpu_st $cpu_ed); do
        allowed_cpus+=($cpu)
    done
else
	IFS=',' read -ra allowed_cpus <<< "$cpus"
	nr_tot_cpus=${#allowed_cpus[@]}
fi


# check whether db_dir exists 
if [[ ! -d $db_dir ]]; then
    echo "The diretory to the RocksDB database is missing...exiting..."
    exit 1
fi

# prepare the database directory 
echo "Before running workloads of $ycsb_workload, $nr_bg_jobs bg jobs, clear the DB directory $db_dir"
echo

echo "your password" | sudo -S rm $db_dir/*
rm $fio_dir/*


# set the stats filename of YCSB
ycsb_out_fname="ycsb-$ycsb_workload-bg-jobs-$nr_bg_jobs-fio-rw-$rw_fio_tenant-bs-$bs_fio_tenant-depth-$depth_fio_tenant-jobs-$nr_fio_tenant-cpus-$cpus"
# load benchmark
echo "Start LOADING workload=$ycsb_workload with $nr_bg_jobs background RocksDB jobs allowed"
taskset --cpu-list $cpus bash $YCSB_DIR/bin/ycsb.sh load rocksdb \
    -P $ycsb_workload_opts  \
    -p rocksdb.dir=$db_dir  \
    -p rocksdb.optionsfile=$db_opts	\
	-p max_background_jobs=$nr_bg_jobs \
	-s > $OUT_DIR/$ycsb_out_fname.load 2>&1
echo "FINISHED LOADING"
echo

## start iostat and cpu stat before running
iostat -x -o JSON -d $nvme_dev 1 > $OUT_DIR/$ycsb_out_fname.io &
iostat_pid=$!

mpstat -I SUM -o JSON -P ALL -u 1 > $OUT_DIR/$ycsb_out_fname.cpu &
mpstat_pid=$!


# run the benchmark
echo "Start RUNNING workload=$ycsb_workload with $nr_bg_jobs background RocksDB jobs allowed and $nr_fio_tenant FIO T-tenants"
echo "These tenants are allowed on cpus=$cpus"
echo

echo "your password" | sudo -S taskset --cpu-list $cpus ionice -c 1 \
    bash $YCSB_DIR/bin/ycsb.sh run rocksdb \
        -P $ycsb_workload_opts  \
        -p rocksdb.dir=$db_dir  \
        -p rocksdb.optionsfile=$db_opts	\
        -p max_background_jobs=$nr_bg_jobs \
        -s > $OUT_DIR/$ycsb_out_fname.run 2>&1 &
ycsb_pgid=$!

## Start background FIO T-tenants
declare -a fio_pgids

for ((job=0; job<$nr_fio_tenant; job++))
do
	cpu_idx=$(( job % nr_tot_cpus ))
	cpu_id=${allowed_cpus[cpu_idx]}

    fio_out_fname="ycsb-$ycsb_workload-bg-jobs-$nr_bg_jobs-fio-rw-$rw_fio_tenant-bs-$bs_fio_tenant-depth-$depth_fio_tenant-tot-jobs-$nr_fio_tenant-$job-th-job-cpus-$cpus.fio"
    setsid fio	--name=global	\
			--gtod_reduce=0	\
			--group_reporting=1	\
			--time_based=1	\
			--direct=1	\
			--ioengine=libaio	\
			--cpus_allowed=$cpu_id	\
			--cpus_allowed_policy=split	\
			--filename=$fio_dir/fio-iter-job-$job	\
			--size=$fio_file_size	\
			\
			--name=fio-iter-job-$job	\
			--rw=$rw_fio_tenant	\
			--bs=$bs_fio_tenant	\
			--iodepth=$depth_fio_tenant	\
			--prioclass=0	\
			--runtime=7200	\
			--numjobs=1	\
			\
			--output=$OUT_DIR/$fio_out_fname &
    fio_pgids[job]=$!
done

# wait for YCSB+RocksDB to finish
wait $ycsb_pgid

# Stop FIO jobs
for ((job=0; job<$nr_fio_tenant; job++))
do
    echo "Killing background FIO T-tenants with PGID=${fio_pgids[job]} of job=$job"
    echo
    pkill -P ${fio_pgids[job]}
    kill ${fio_pgids[job]}
done

echo "Killing background iostat with PID=$iostat_pid"
echo
kill $iostat_pid

echo "Killing background mpstat with PID=$mpstat_pid"
echo
kill $mpstat_pid

echo "Copying RocksDB log file..."
echo
cp $db_dir/LOG $OUT_DIR/$ycsb_out_fname.LOG
