#! /usr/bin/env bash

# This script uses FIO to continuously add running jobs, e.g., Job 1 -> [0:60], Job 2 -> [10: 60], Jobs 3 -> [20:60], etc.
# In this way, the pressure is increasing within a certain time window

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

EVAL_DIR=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
BASE_OUT_DIR=$EVAL_DIR/results-fio-mt-sh

## Pre-defined variables
interval=600 # the interval between each round of increasing throughput pressure

## Arguments
### const tenants
nr_const_tenant=''	# number of constant tenants
rw_const_tenant=''	# R/W type of constant tenants
bs_const_tenant=''	# block isze
depth_const_tenant=''	# I/O depth
prio_const_tenant=''	# I/O priority

### continuous tenants
nr_cont_tenant=''
rw_cont_tenant=''
bs_cont_tenant=''
depth_cont_tenant=''
prio_cont_tenant=''

nvme_dev=''
cpus=''
sys=''
res_dir=''

while (( "$#" )); do
	case "$1" in
	  --nr_const)
		nr_const_tenant="$2"
		shift 2
		;;
	  --rw_const)
		rw_const_tenant="$2"
		shift 2
		;;
	  --bs_const)
	  	bs_const_tenant="$2"
	  	shift 2
	  	;;
	  --depth_const)
		depth_const_tenant="$2"
		shift 2
		;;
	  --prio_const)
	  	prio_const_tenant="$2"
		shift 2
		;;
	  --nr_cont)
		IFS=',' read -a nr_cont_tenant <<< "$2"
		shift 2
		;;
  	  --rw_cont)
	  	rw_cont_tenant="$2"
	  	shift 2
	  	;;
	  --bs_cont)
	  	bs_cont_tenant="$2"
	  	shift 2
	  	;;
	  --depth_cont)
		depth_cont_tenant="$2"
		shift 2
		;;
	  --prio_cont)
	  	prio_cont_tenant="$2"
		shift 2
		;;
 	  --dev)
	  	nvme_dev="$2"
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

tot_cont_iters=${#nr_cont_tenant[@]}

## create continuously running fio jobs
cpu_stat_fname="fio-const-cont-rw-$rw_const_tenant-$rw_cont_tenant-bs-$rw_const_tenant-$rw_cont_tenantt-depth-$depth_const_tenant-$depth_cont_tenant-jobs-$nr_const_tenant-$(echo ${nr_cont_tenant[@]} | tr ' ' '_')-prio-$prio_const_tenant-$prio_cont_tenant-interval-$interval-cpus-$cpus.cpu"
io_stat_fname="fio-const-cont-rw-$rw_const_tenant-$rw_cont_tenant-bs-$rw_const_tenant-$rw_cont_tenantt-depth-$depth_const_tenant-$depth_cont_tenant-jobs-$nr_const_tenant-$(echo ${nr_cont_tenant[@]} | tr ' ' '_')-prio-$prio_const_tenant-$prio_cont_tenant-interval-$interval-cpus-$cpus.io"

iostat -x -o JSON -d $nvme_dev 1 $((interval * tot_cont_iters)) > $OUT_DIR/$io_stat_fname &
# We collect CPU usage of all cpus as Daredevil enables flexible routing
mpstat -I SUM -o JSON -P ALL -u 1 $((interval * tot_cont_iters)) > $OUT_DIR/$cpu_stat_fname &

for ((iter=0; iter<$tot_cont_iters; iter++))
do
	nr_cont=${nr_cont_tenant[$iter]}
	cont_runtime=$((interval * (tot_cont_iters - iter)))

	fio_out_const_fname="fio-const-rw-$rw_const_tenant-bs-$bs_const_tenant-depth-$depth_const_tenant-jobs-$nr_const_tenant-prio-$prio_const_tenant-iter-$iter-runtime-$interval-cpus-$cpus.fio"
	fio_out_cont_fname="fio-cont-rw-$rw_cont_tenant-bs-$bs_cont_tenant-depth-$depth_cont_tenant-jobs-$nr_cont-prio-$prio_cont_tenant-iter-$iter-runtime-$cont_runtime-cpus-$cpus.fio"

	echo "Running iteration=$iter with rw=$rw_const_tenant,$rw_cont_tenant;" \
     "bs=$bs_const_tenant,$bs_cont_tenant; iodepth=$depth_const_tenant,$depth_cont_tenant;" \
     "jobs=$nr_const_tenant,$nr_cont; dev=${nvme_dev#/dev/}; interval=$interval; cont_runtime=$cont_runtime"
	echo

	echo "Const file=$fio_out_const_fname, Cont file=$fio_out_cont_fname"
	echo

	sudo fio --name=global	\
		--gtod_reduce=0	\
		--group_reporting=1	\
		--time_based=1	\
		--direct=1	\
		--ioengine=libaio	\
		--cpus_allowed=$cpus	\
		--cpus_allowed_policy=split	\
		--filename=$nvme_dev	\
		--size=1G	\
		\
		--name=iter-$iter-const-tenants	\
		--rw=$rw_const_tenant	\
		--bs=$bs_const_tenant	\
		--iodepth=$depth_const_tenant	\
		--prioclass=$prio_const_tenant	\
		--runtime=$interval	\
		--numjobs=$nr_const_tenant	\
		--output=$OUT_DIR/$fio_out_const_fname &
	
	sudo fio --name=global	\
		--gtod_reduce=0	\
		--group_reporting=1	\
		--time_based=1	\
		--direct=1	\
		--ioengine=libaio	\
		--cpus_allowed=$cpus	\
		--cpus_allowed_policy=split	\
		--filename=$nvme_dev	\
		--size=1G	\
		\
		--name=iter-$iter-cont-tenants	\
		--rw=$rw_cont_tenant	\
		--bs=$bs_cont_tenant	\
		--iodepth=$depth_cont_tenant	\
		--prioclass=$prio_cont_tenant	\
		--runtime=$cont_runtime	\
		--numjobs=$nr_cont	\
		\
		--output=$OUT_DIR/$fio_out_cont_fname &

	sleep $interval

done

wait
