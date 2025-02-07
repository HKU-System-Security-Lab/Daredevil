#! /usr/bin/env bash

# This script runs the multi-namespace test

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

sub_eval_sys_dir=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
parent_eval_dir=$(dirname $sub_eval_sys_dir)
parent_eval_script='eval_fio_per_host_test_base.sh'
if [[ ! -f "$parent_eval_dir/$parent_eval_script" ]]; then
	echo "The require test script: $parent_eval_script does not exist within $parent_eval_dir"
	exit 1
fi


nvme_dev='/dev/nvme0' # the device of the target NVMe SSD, e.g., /dev/nvme0
nr_ns_list=(4 8 12 16)    # the number of namespace to be created
nr_lat_ns_list=(1 2 3 4) # the number of namespaces for L-tenants
nr_iters=4

rw_lat_tenant='randread'
bs_lat_tenant='4k'
depth_lat_tenant='1'

rw_thput_tenant='randwrite'
bs_thput_tenant='128k'
depth_thput_tenant=32

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

test_file=$(basename "$0")

for cpu in ${cpus[@]}
do
    for (( i=0; i<$nr_iters; i++))
    do
        nr_ns=${nr_ns_list[$i]}
        nr_lat_ns=${nr_lat_ns_list[$i]}

        nr_lat_tenant=$(( nr_lat_ns * 2 ))
        nr_thput_tenant=$(( nr_ns * 8 - nr_lat_ns * 8))

	    echo "iter=$i cpu=$cpu uses nr_lat=$nr_lat_tenant and nr_thput=$nr_thput_tenant with nr_ns=$nr_ns, nr_lat_ns=$nr_lat_ns"

        $parent_eval_dir/$parent_eval_script    \
            --dev $nvme_dev \
            --ns_num    $nr_ns  \
            --ns_lat_num    $nr_lat_ns   \
            --nr_lat_tenant $nr_lat_tenant \
            --nr_thput_tenant   $nr_thput_tenant  \
            --rw_lat_tenant $rw_lat_tenant    \
            --rw_thput_tenant   $rw_thput_tenant   \
            --bs_lat_tenant $bs_lat_tenant  \
            --bs_thput_tenant   $bs_thput_tenant    \
            --depth_lat_tenant  $depth_lat_tenant   \
            --depth_thput_tenant    $depth_thput_tenant \
            --cpus  $cpu    \
            --sys   $sys    \
            --res_dir   ${test_file%.sh}/nr-ns-$nr_ns-nt-lat-ns-$nr_lat_ns/$cpu
    done
done
