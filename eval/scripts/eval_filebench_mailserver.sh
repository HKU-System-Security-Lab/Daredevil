#! /usr/bin/env bash

## This script tests the **mailserver** workloads of filebench

## The FIO jobs use sequetial writes to simulate background streaming I/O workloads, consistent with filebench/workloads/stream*

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

sub_eval_sys_dir=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
parent_eval_dir=$(dirname $sub_eval_sys_dir)
parent_eval_script='eval_filebench_test_cont.sh'
if [[ ! -f "$parent_eval_dir/$parent_eval_script" ]]; then
	echo "The require test script: $parent_eval_script does not exist within $parent_eval_dir"
	exit 1
fi

## Filebench arguments
filebench_dir=$parent_eval_dir/test-filebench/mailserver
filebench_name='mailserver'
filebench_file=''

## FIO arguments
nr_fio='8'
rw_fio='write'
bs_fio='256k'
depth_fio='32'

## Environments
interval=600
dev='/dev/nvme0n1'
cpus=('0-3')
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

## Preliminary preparation
## copy the WML file here
filebench_file=filebench-mailserver.f

sed -i "s#set \$dir=.*#set \$dir=$filebench_dir#g" $sub_eval_sys_dir/$filebench_file  
sed -i "s/set \$runtime=.*/set \$runtime=$interval/g" $sub_eval_sys_dir/$filebench_file

test_file=$(basename "$0")

for cpu in ${cpus[@]}
do
	echo "Start running filebench with $filebench_name cpus=$cpu"
	
	$parent_eval_dir/$parent_eval_script	\
		--interval	$interval	\
		--dir	$filebench_dir	\
		--bench_file	$sub_eval_sys_dir/$filebench_file	\
		--bench_name	$filebench_name	\
		--nr_fio	$nr_fio	\
		--rw_fio	$rw_fio	\
		--bs_fio	$bs_fio	\
		--depth_fio	$depth_fio	\
		--cpus	$cpu	\
		--sys	$sys	\
		--nvme_dev	$dev	\
		--res_dir	${test_file%.sh}/cpus-$cpu
	
	sleep 30
done

