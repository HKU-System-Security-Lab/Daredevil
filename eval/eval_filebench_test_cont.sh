#! /usr/bin/env bash

# This scripts uses filebench to run simulated read-world fileserver I/Os, which are regarded as L-tenants.

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

EVAL_DIR=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
BASE_OUT_DIR=$EVAL_DIR/results-filebench-mt-sh

# Per-run interval
interval=''

## Arguments for filebench
bench_dir=''	# the directory to write to
bench_file=''	# the filebench benchmark WML file, only including fileserver, mailserver, and webserver
bench_name=''	# the name of the filebench

## Arguments for FIO
nr_fio_tenant=''	# number of FIO T-tenants
rw_fio_tenant=''	# type of FIO T-tenants
bs_fio_tenant=''
depth_fio_tenant=''

## Environment arguments
cpus=''
sys=''	# evaluated system
res_dir=''
nvme_dev=''

## Predefined variable
fio_file_size=32G

while (( "$#" )); do
	case "$1" in
	  --interval)
		interval="$2"
		shift 2
		;;
	  --dir)
		bench_dir="$2"
		shift 2
		;;
	  --bench_file)
		bench_file="$2"
		shift 2
		;;
	  --bench_name)
		bench_name="$2"
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

echo "cpus=$cpus nr_tot_cpus=$nr_tot_cpus"

tot_fio_iters=${#nr_fio_tenant[@]}

for ((iter=0; iter<$tot_fio_iters; iter++))
do
	nr_fio=${nr_fio_tenant[$iter]}
	fio_runtime=$interval
	filebench_pid=''
	filebench_out_fname="filebench-$bench_name-iter-$iter-fio-rw-$rw_fio_tenant-bs-$bs_fio_tenant-depth-$depth_fio_tenant-jobs-$nr_fio-cpus-$cpus.filebench"

	# clear the target benchdir first
	sudo rm $bench_dir/fio-*

	io_stat_fname="filebench-$bench_name-iter-$iter-fio-rw-$rw_fio_tenant-bs-$bs_fio_tenant-depth-$depth_fio_tenant-jobs-$nr_fio-cpus-$cpus-interval-$interval.io"
	cpu_stat_fname="filebench-$bench_name-iter-$iter-fio-rw-$rw_fio_tenant-bs-$bs_fio_tenant-depth-$depth_fio_tenant-jobs-$nr_fio-cpus-$cpus-interval-$interval.cpu"

	## iostats
	iostat -x -o JSON -d $nvme_dev 1 $interval > $OUT_DIR/$io_stat_fname &
	## cpustats
	mpstat -I SUM -o JSON -P ALL -u 1 $interval > $OUT_DIR/$cpu_stat_fname &


	echo "Start running filebench=$bench_name iter=$iter/tot_iters=$tot_fio_iters with nr_fio=$nr_fio, fio_runtime=$fio_runtime, rw_fio=$rw_fio_tenant, bs_fio=$bs_fio_tenant, depth_fio=$depth_fio_tenant, cpus=$cpus"

	# Start filebench workload first
	taskset --cpu-list $cpus sudo ionice -c 1 filebench -f $bench_file > $OUT_DIR/$filebench_out_fname &
	filebench_pid=$!
	
	# monitor the I/O stats of filebench
	filebench_pid_iostat_fname="filebench-$bench_name-fio-rw-$rw_fio_tenant-bs-$bs_fio_tenant-depth-$depth_fio_tenant-jobs-$nr_fio-runtime-$interval-pid-$filebench_pid.pidstat"
	# Temporary masked out: pidstat -dl -p $filebench_pid 1 -T ALL -t > $OUT_DIR/$filebench_pid_iostat_fname &

	# Start FIO workload
	for ((job=0; job<$nr_fio; job++))
	do
		cpu_idx=$(( job % nr_tot_cpus ))
		cpu_id=${allowed_cpus[cpu_idx]}

		echo "job=$job, nr_fio=$nr_fio, cpu_idx=$cpu_idx, cpu_id=$cpu_id, allowed_cpus=${allowed_cpus[@]}"

		fio_out_fname="filebench-$bench_name-iter-$iter-fio-rw-$rw_fio_tenant-bs-$bs_fio_tenant-depth-$depth_fio_tenant-jobs-$job-cpus-$cpus-used-cpu-$cpu_id-runtime-$fio_runtime.fio"

		sudo fio	--name=global	\
			--gtod_reduce=0	\
			--group_reporting=1	\
			--time_based=1	\
			--direct=1	\
			--ioengine=libaio	\
			--cpus_allowed=$cpu_id	\
			--cpus_allowed_policy=split	\
			--filename=$bench_dir/fio-iter-$iter-job-$job	\
			--size=$fio_file_size	\
			\
			--name=fio-iter-$iter-job-$job	\
			--rw=$rw_fio_tenant	\
			--bs=$bs_fio_tenant	\
			--iodepth=$depth_fio_tenant	\
			--prioclass=0	\
			--runtime=$fio_runtime	\
			--numjobs=1	\
			\
			--output=$OUT_DIR/$fio_out_fname &		
	done

	sleep $((interval + 30))
done

wait
