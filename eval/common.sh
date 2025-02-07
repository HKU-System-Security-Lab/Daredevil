#! /usr/bin/env bash

function precond(){
	devname=$1
	type=$2

	if [[ $type == "rand" ]]; then
		sudo fio --name=precond-rand	\
			--filename=$devname	\
		  	--direct=1	\
			--loops=2	\
			--runtime=300	\
			--time_based=1	\
			--ioengine=libaio	\
			--rw=randwrite	\
			--numjobs=4	\
			--bs=64k	\
			--iodepth=256	\
			--size=100%

	elif [[ $type == "seq" ]]; then
		sudo fio --name=precond-seq	\
			--filename=$devname	\
			--direct=1	\
			--loops=2	\
			--runtime=300	\
			--time_based=1	\
			--ioengine=libaio	\
			--rw=write	\
			--bs=256k	\
			--iodepth=16	\
			--size=100%
	else
		echo "Wrong Args: $devname and $type"	
	fi
}

function io_stats_collect(){
	runtime=$1
	out_dir=$2
	out_fname=$3

	iostat -x -o JSON 1 $runtime > $out_dir/$out_fname.io &
}
