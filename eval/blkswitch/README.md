# blk-switch: Rearchitecting Linux Storage Stack for μs Latency and High Throughput

This directory contains the integrated version of blk-switch for Linux kernel version 6.1.53 as this is the kernel version used in the Daredevil project as the vanilla kernel and blk-switch was originally implemented in kernel 5.4.43.

In this integrated version, the functionality of blk-switch is well preserved as the **only modification is the adaptation of different kernel function interfaces between 6.1.53 and 5.4.43**. 
However, as blk-switch also considers the NIC device as one of its targets, which is out of the scope of the Daredevil project, and integrates an optimized kernel network stack, [i10](https://www.usenix.org/conference/nsdi20/presentation/hwang), in its implementation, **we discard the i10-related implemtation during the integration and only preserve its optimization for the local storage**.

For the original documentation and implementation, please refer to its [original github repository](https://github.com/resource-disaggregation/blk-switch) for detailed information.
