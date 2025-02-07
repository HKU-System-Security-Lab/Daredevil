#! /usr/bin/env bash

# This script tests the RockDB + YCSB benchmark with increasing pressure of T-tenants

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

sub_eval_sys_dir=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
parent_eval_dir=$(dirname $sub_eval_sys_dir)
parent_eval_script='eval_rocksdb_ycsb_test_cont.sh'
if [[ ! -f "$parent_eval_dir/$parent_eval_script" ]]; then
	echo "The require test script: $parent_eval_script does not exist within $parent_eval_dir"
	exit 1
fi

## RocksDB arguments
db_dir=$parent_eval_dir/mnt/rocksdb
db_opts=$parent_eval_dir/rocksdb-ycsb.ini
nr_bg_jobs=4

## YCSB arguments
workloads=('workloada' 'workloadb' 'workloade' 'workloadf')

## FIO arguments
nr_fio=('8')
rw_fio='write'
bs_fio='512k'
depth_fio='16'
fio_dir=$parent_eval_dir/mnt/fio-tenants

## Environments
cpus=('0-3')
dev='/dev/nvme0n1'
sys='' # The name of the target, i.e., Dardevil, blkswitch, or vanilla
while (( "$#" )); do
	case "$1" in
	  --sys)
		sys="$2"
		shift 2
		;;
      *)
		usage
		exit 1
    esac
done

if [[ ! -d $db_dir ]];then
	echo "$db_dir doesn't exist yet, creating one..."
	mkdir -p $db_dir
fi

if [[ ! -d $fio_dir ]];then
	echo "$fio_dir doesn't exist yet, creating one..."
	mkdir -p $fio_dir
fi


test_file=$(basename "$0")

for workload in ${workloads[@]}
do
	for cpu in ${cpus[@]}
	do
		for fio_job in ${nr_fio[@]}
		do
			$parent_eval_dir/$parent_eval_script	\
				--db_dir	$db_dir	\
				--db_opts	$db_opts	\
				--bg_jobs	$nr_bg_jobs	\
				--workload	$workload	\
				--nr_fio	$fio_job	\
				--rw_fio	$rw_fio	\
				--bs_fio	$bs_fio	\
				--depth_fio	$depth_fio	\
				--fio_dir	$fio_dir	\
				--cpus	$cpu	\
				--sys	$sys	\
				--nvme_dev	$dev	\
				--res_dir ${test_file%.sh}/$workload/cpus-$cpu/fio-job-$fio_job
		done
		sleep 30
	done

done
