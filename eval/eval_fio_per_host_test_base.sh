#! /usr/bin/env bash

# This script tests the multi-host scenario with each type of tenants occupying different hosts (namespaces) of an NVMe SSD

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

EVAL_DIR=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
BASE_OUT_DIR=$EVAL_DIR/results-fio-mt-mh-separate

# Predefined
interval=600

## namespace arguments
nvme_dev='' # the device of the target NVMe SSD, e.g., /dev/nvme0
nr_ns=''    # the number of namespace to be created
nr_lat_ns='' # the number of namespaces for L-tenants
nr_thput_ns=''  # number of namespaces for T-tenants, equal to nr_thput_ns = nr_ns - nr_lat_ns

## FIO arguments
nr_lat_tenant=''
rw_lat_tenant=''
bs_lat_tenant=''
depth_lat_tenant=''

nr_thput_tenant=''
rw_thput_tenant=''
bs_thput_tenant=''
depth_thput_tenant=''

## Environment arguments
cpus=''
sys=''
res_dir=''

while (( "$#" )); do
	case "$1" in
        --dev)
            nvme_dev="$2"
            shift 2
            ;;
        --ns_num)
            nr_ns="$2"
            shift 2
            ;;
        --ns_lat_num)
            nr_lat_ns="$2"
            shift 2
            ;;
        --nr_lat_tenant)
            nr_lat_tenant="$2"
            shift 2
            ;;
        --nr_thput_tenant)
            nr_thput_tenant="$2"
            shift 2
            ;;
        --rw_lat_tenant)
            rw_lat_tenant="$2"
            shift 2
            ;;
        --rw_thput_tenant)
            rw_thput_tenant="$2"
            shift 2
            ;;
        --bs_lat_tenant)
            bs_lat_tenant="$2"
            shift 2
            ;;
        --bs_thput_tenant)
            bs_thput_tenant="$2"
            shift 2
            ;;
        --depth_lat_tenant)
            depth_lat_tenant="$2"
            shift 2
            ;;
        --depth_thput_tenant)
            depth_thput_tenant="$2"
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

nr_thput_ns=$(( nr_ns - nr_lat_ns ))

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

## Creating namespaces
echo "Calling 'manage_nvme_ns.sh' script to create $nr_ns namespaces on $nvme_dev"
bash $EVAL_DIR/manage_nvme_ns.sh --dev $nvme_dev --ns_num $nr_ns
echo "Finished creating namespaces"
echo

for (( i=0; i<$nr_lat_tenant;i++))
do
    nsid=$(( i % nr_lat_ns + 1 ))
    ns_dev=$nvme_dev"n"$nsid

    cpu_idx=$(( i % nr_tot_cpus ))
    cpu_id=${allowed_cpus[cpu_idx]}

    echo "L-tenant $i runs on nsid=$nsid ns_dev=$ns_dev with allowed cpu=$cpu_id"

    fio_lat_fname="fio-lat-rw-$rw_lat_tenant-bs-$bs_lat_tenant-depth-$depth_lat_tenant-jobs-1-prio-1-idx-$i-runtime-$interval-cpus-$cpus-ns-used-cpu-$cpu_id-${ns_dev#/dev/}.fio"

    echo "Output file $i=$fio_lat_fname"

    sudo fio --name=global	\
		--gtod_reduce=0	\
		--group_reporting=1	\
		--time_based=1	\
		--direct=1	\
		--ioengine=libaio	\
		--cpus_allowed=$cpu_id	\
		--cpus_allowed_policy=split	\
		--filename=$ns_dev	\
		--size=1G	\
		\
		--name=L-tenant-$i-ns-dev-$ns_dev	\
		--rw=$rw_lat_tenant	\
		--bs=$bs_lat_tenant	\
		--iodepth=$depth_lat_tenant	\
		--prioclass=1	\
		--runtime=$interval	\
		--numjobs=1	\
		--output=$OUT_DIR/$fio_lat_fname &
done

echo

for (( i=0; i<$nr_thput_tenant;i++))
do
    nsid=$(( i % nr_thput_ns + nr_lat_ns + 1 ))
    ns_dev=$nvme_dev"n"$nsid
    
    cpu_idx=$(( i % nr_tot_cpus ))
    cpu_id=${allowed_cpus[cpu_idx]}

    echo "T-tenant $i runs on nsid=$nsid ns_dev=$ns_dev with allowed cpu=$cpu_id"

    fio_thput_fname="fio-thput-rw-$rw_lat_tenant-bs-$bs_thput_tenant-depth-$depth_thput_tenant-jobs-1-prio-0-idx-$i-runtime-$interval-cpus-$cpus-used-cpu-$cpu_id-ns-${ns_dev#/dev/}.fio"

    echo "Output file $i=$fio_thput_fname"
    
    sudo fio --name=global	\
		--gtod_reduce=0	\
		--group_reporting=1	\
		--time_based=1	\
		--direct=1	\
		--ioengine=libaio	\
		--cpus_allowed=$cpu_id	\
		--cpus_allowed_policy=split	\
		--filename=$ns_dev	\
		--size=1G	\
		\
		--name=T-tenant-$i-ns-dev-$ns_dev	\
		--rw=$rw_thput_tenant	\
		--bs=$bs_thput_tenant	\
		--iodepth=$depth_thput_tenant	\
		--prioclass=0	\
		--runtime=$interval	\
		--numjobs=1	\
		--output=$OUT_DIR/$fio_thput_fname &
done

wait

echo
