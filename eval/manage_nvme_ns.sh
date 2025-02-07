#! /usr/bin/env bash

# This script is used to create, modify, or delete the namespaces of a given device.
# Parts of this script is inherited from https://narasimhan-v.github.io/2020/06/12/Managing-NVMe-Namespaces.html
# For Samsung PM1735, its block size is 512

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

nvme_dev='' # the device of the target NVMe SSD, e.g., /dev/nvme0
nr_ns=''    # the number of namespace to be created

max_ns=''   # the maximum namespace allowed
block_size='' # the block size of the target device

controller=''   # the controller id of the device 

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
        *)
            usage
            exit 1
    esac
done

if [[ -z $nvme_dev || -z $nr_ns ]]; then
    echo "Missing arguments of nvme_dev or nr_ns, can not continue. Exiting...."
    exit 1
fi

echo "Start formatting nvme device $nvme_dev"
echo
sudo nvme format $nvme_dev -n 0xffffffff --lbaf=0

controller=$(sudo nvme id-ctrl $nvme_dev | grep ^cntlid | awk '{print $NF}')

echo "Getting the block size of the device"
if [[ ! -b $nvme_dev"n1" ]]; then
    echo "No namespaces exist for the device $nvme_dev, creating a temporary one"
    sudo nvme delete-ns $nvme_dev --namespace-id=0xffffffff
    sudo nvme create-ns $nvme_dev --nsze=625123397 --ncap=625123397 --flbas=0 --dps=0
    sudo nvme attach-ns $nvme_dev --namespace-id=1 --controllers=$controller
fi
block_size=$((2**`sudo nvme id-ns $nvme_dev"n1" | grep "in use" | awk '{print $5}' | awk -F: '{print $NF}'`))
echo "NVMe device $nvme_dev has block size=$block_size"
echo

echo "Deleting all existing namespaces"
sudo nvme delete-ns $nvme_dev -n 0xFFFFFFFF
sleep 5

echo "Showing current nvme list"
sudo nvme list
echo

max_ns=$(sudo nvme id-ctrl $nvme_dev | grep ^nn | awk '{print $NF}')
echo "$nvme_dev's maximum supported ns=$max_ns"

max_size=$(sudo nvme id-ctrl $nvme_dev | grep -i tnvmcap | awk '{print $NF}')
echo "$nvme_dev's maximum capacity=$max_size"

used_max_size=$(( $max_size * 80 / 100 ))
echo "Provision $nvme_dev by using only 80% capacity, truncating its max_size=$used_max_size"

max_nr_blocks=$(expr $used_max_size / $block_size)
echo "$nvme_dev now has maximum blocks=$max_nr_blocks for usage"

per_ns_blocks=$(expr $max_nr_blocks / $nr_ns)
echo "Per namespace blocks=$per_ns_blocks"

echo

echo "Now creating $nr_ns namespaces with each having $per_ns_blocks blocks of size $block_size for $nvme_dev"
for ((i=1; i<=$nr_ns; i++))
do
    echo "Creating namespace $i, i.e., $nvme_dev"n"$i"

    echo "Executing command: nvme create-ns $nvme_dev --nsze=$per_ns_blocks --ncap=$per_ns_blocks --flbas=0 --dps=0"
    sudo nvme create-ns $nvme_dev --nsze=$per_ns_blocks --ncap=$per_ns_blocks --flbas=0 --dps=0

    echo "Executing command: nvme attach-ns $nvme_dev --namespace-id=$i --controllers=$controller"
    sudo nvme attach-ns $nvme_dev --namespace-id=$i --controllers=$controller

    sleep 5

    sudo nvme id-ns "$nvme_dev"n"$i" | grep -E "nsze|nvmcap"

    echo "Finished creating and attach namespace $i for $nvme_dev"
done

sudo nvme ns-rescan $nvme_dev
sudo nvme list 
