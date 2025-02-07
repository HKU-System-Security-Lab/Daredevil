#! /usr/bin/env bash

# This script evaluates the system's performance with continuously increasing pressure of T-tenants

# Pattern: 4 jobs 4K randread + 4-32 jobs 128K randwrite (You may change this pattern to suit your customized needs)

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

sub_eval_sys_dir=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
parent_eval_dir=$(dirname $sub_eval_sys_dir)
parent_eval_script='eval_fio_cont_test_base.sh'
if [[ ! -f "$parent_eval_dir/$parent_eval_script" ]]; then
	echo "The require test script: $parent_eval_script does not exist within $parent_eval_dir"
	exit 1
fi

source $parent_eval_dir/common.sh

nr_const=4
rw_const='randread'
bs_const='4k'
depth_const=1
prio_const=1

nr_cont='4,8,12,16,20,24,32'
rw_cont='randwrite'
bs_cont='128k'
depth_cont=32
prio_cont=0

dev='/dev/nvme0n1'
cpus=('0-1' '0-3' '0-7')
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

test_file=$(basename "$0")

for cpu in ${cpus[@]}
do
    echo "Start preconditional writes of type 'rand'"
    echo

    precond $dev 'rand'

    echo "Finished preconditional writes, sleep for some time..."
    echo
    sleep 30

    $parent_eval_dir/$parent_eval_script \
        --nr_const $nr_const    \
        --rw_const $rw_const    \
        --bs_const $bs_const    \
        --depth_const  $depth_const \
        --prio_const    $prio_const \
        \
        --nr_cont $nr_cont    \
        --rw_cont $rw_cont    \
        --bs_cont $bs_cont    \
        --depth_cont  $depth_cont \
        --prio_cont    $prio_cont \
        \
        --dev $dev  \
        --cpus $cpu \
        --sys $sys  \
        --res_dir ${test_file%.sh}
done
