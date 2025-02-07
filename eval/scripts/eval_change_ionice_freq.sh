#! /usr/bin/env bash

# This script evaluates the overheads of changing tenants' ionice values under different frequency, **with only one tenant running**
# This measures the basic performance overheads of changing a single tenant's ionice

exit_test(){
	echo "Receving SIGINT, exiting this script"
	exit 1
}

trap 'exit_test' SIGINT

CUR_DIR=$(dirname $(readlink -f ${BASH_SOURCE[0]}))
EVAL_DIR=$(dirname $CUR_DIR)

sys=''  # The name of the target, i.e., Dardevil, blkswitch, or vanilla
tag=''  # User-specific tags
while (( "$#" )); do
	case "$1" in
	  --sys)
		sys="$2"
		shift 2
		;;
	  --tag)
		tag="$2"
		shift 2
		;;
      *)
		usage
		exit 1
    esac
done

# Check if the targeted sys matches any of the allowed ones
if [[ ! "$sys" =~ ^(vanilla|daredevil|blkswitch)$ ]]; then
    echo "The passed in sys: $sys does not match any of vanilla|daredevil|blkswitch"
    exit 1
fi

# Update interval: 1s, 500ms, 250ms, 100ms, 50ms, 25ms, 20ms, 15ms, 12.5ms, 10ms, 1ms, 500us, 100us, 50us, 10us
frequency="60 120 240 600 1200 2400 3000 4000 4800 6000 60000 120000 600000 1200000 6000000"
per_minute_secs=60 # secs per minute
runtime_min=2  # 2 minutes
runtime_secs=$(($runtime_min * $per_minute_secs)) # the duration of running FIO jobs, 5 minutes = 300 secs
nvme_dev='/dev/nvme0n1' # the target NVMe device

if [[ -z "$tag" ]];
then
	res_dir=$EVAL_DIR/result/eval/ionice-freq/single-tenant/$sys   # the result directory
else
	res_dir=$EVAL_DIR/result/eval/ionice-freq/single-tenant/$sys/$tag   # the result directory
fi
# Check if the directory exists
if [[ ! -d "$res_dir" ]]; then
	echo "The resulting directory is not present, creating $res_dir"
	mkdir -p $res_dir
fi

rw='randread'
bs='4k'
iodepth=1

# Uncomment the following to launch simulated T-tenants
# rw='read'
# bs='256k'
# iodepth=64

for freq in $frequency
do
    # The result file
    ionice_freq_single_result_file="fio-ionice-freq-single-rw-$rw-bs-$bs-iodepth-$iodepth-freq-$freq-runtime-$runtime_secs.fio"

    # Launch the target fio process
    sudo fio --name=global  \
            --gtod_reduce=0	\
            --group_reporting=0	\
            --time_based=1	\
            --direct=1	\
            --ioengine=libaio	\
            --cpus_allowed_policy=split	\
            --filename=$nvme_dev \
            --size=100%   \
            --output=$res_dir/$ionice_freq_single_result_file  \
            \
            --name=ionice-change-single    \
            --rw=$rw   \
            --bs=$bs    \
            --iodepth=$iodepth	\
            --runtime=$runtime_secs    \
            --numjobs=1 \
            --cpus_allowed=0 &

    if [[ $freq -eq 0 ]]; then
        wait
    else
        # Get the target PID of the process that has its ionice changed
        target_pid=''   # The PID of the target process of changing frequencies

        fio_parent_pid=$!
        sleep 5
        fio_launch_pid=$(pgrep -P $fio_parent_pid)  # The pid of the process to launch potential I/O processes
        launched_io_child_pids=($(pgrep -P $fio_launch_pid))   # The pid list of launched I/O processes

        potential_target_pid=${launched_io_child_pids[0]}   # Select a potential one
        potential_target_childs=($(pgrep -P $potential_target_pid)) # Whether the potential one has launched any further processes, 

        if [[ ${#potential_target_childs[@]} -eq 0 ]]; then
            # The potential one does not launch any further child I/O process
            target_pid=$potential_target_pid
        else
            # The potential one has launched further child processes, use the child process
            target_pid=${potential_target_childs[0]}
        fi


        echo "Parent: $fio_parent_pid launch: $fio_launch_pid launched_io_childs: ${launched_io_child_pids[@]}"
        echo "potential target pid: $potential_target_pid Potential target's child's: ${potential_target_childs[@]}"
        echo "Target pid: $target_pid"

        # Calculate the frequency accordingly
        sleep_secs=$(echo "scale=6; $per_minute_secs / $freq" | bc)
        iters=$(($freq * $runtime_min))

        for ((i=0; i<$iters; i++)); do
            if ps -p $target_pid > /dev/null
            then
                sudo ionice -c $((i%2)) -p $target_pid
                sleep $sleep_secs
            else
                break
            fi
        done
    fi
done
