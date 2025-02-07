#! /usr/bin/env bash

# This script is used to reset the NVMe SSD back to its initial namespace usage

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

nvme_dev='' # the device of the target NVMe SSD, e.g., /dev/nvme0
block_size='' # the block size of the target device

while (( "$#" )); do
	case "$1" in
        --dev)
            nvme_dev="$2"
            shift 2
            ;;
        --block_size)
            block_size="$2"
            shift 2
            ;;
        *)
            usage
            exit 1
    esac
done

controller=$(sudo nvme id-ctrl $nvme_dev | grep ^cntlid | awk '{print $NF}')
echo "controller=$controller"

if [[ -z $block_size ]]; then
    if [[ -b $nvme_dev"n1" ]]; then
        block_size=$((2**`sudo nvme id-ns $nvme_dev"n1" | grep "in use" | awk '{print $5}' | awk -F: '{print $NF}'`))
    else
        echo "No namespaces exist for the device $nvme_dev, creating a temporary one"
        sudo nvme delete-ns $nvme_dev --namespace-id=0xffffffff
        sudo nvme create-ns $nvme_dev --nsze=625123397 --ncap=625123397 --flbas=0 --dps=0
        sudo nvme attach-ns $nvme_dev --namespace-id=1 --controllers=$controller
        block_size=$((2**`sudo nvme id-ns $nvme_dev"n1" | grep "in use" | awk '{print $5}' | awk -F: '{print $NF}'`))        
    fi
fi

echo "Block size is $block_size"

# delete all ns
sudo nvme delete-ns $nvme_dev --namespace-id=0xffffffff

# reset back the device
max_size=$(sudo nvme id-ctrl $nvme_dev | grep -i tnvmcap | awk '{print $NF}')
max_nr_blocks=$(expr $max_size / $block_size)

sudo nvme create-ns $nvme_dev --nsze=$max_nr_blocks --ncap=$max_nr_blocks --flbas=0 --dps=0
sudo nvme attach-ns $nvme_dev --namespace-id=1 --controllers=$controller

sudo nvme ns-rescan $nvme_dev
sudo nvme list