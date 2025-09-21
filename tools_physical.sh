#!/bin/bash

# update parameters
PCI_PATH="03:00.1"

# fixed parameters
Pages=2048
HUGEPGSZ=`cat /proc/meminfo  | grep Hugepagesize | cut -d : -f 2 | tr -d ' '`

#
# Loads vfio-pci.
#
load_vfio_pci_module()
{
    modinfo vfio-pci > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "## ERROR: vfio-pci module not found in this kernel."
        echo "       Please make sure your kernel has VFIO support."
        return 1
    fi

    echo $passwd | sudo -S modprobe vfio-pci
    if [ $? -ne 0 ]; then
        echo "## ERROR: Failed to load vfio-pci"
        return 1
    fi
}

#
# Creates hugepage filesystem.
#
create_mnt_huge()
{
    echo "Creating /mnt/huge and mounting as hugetlbfs"
    echo $passwd | sudo -S mkdir -p /mnt/huge

    grep -s '/mnt/huge' /proc/mounts > /dev/null
    if [ $? -ne 0 ] ; then
        echo $passwd | sudo -S mount -t hugetlbfs nodev /mnt/huge
    fi
}

#
# Removes hugepage filesystem.
#
remove_mnt_huge()
{
    echo "Unmounting /mnt/huge and removing directory"
    grep -s '/mnt/huge' /proc/mounts > /dev/null
    if [ $? -eq 0 ] ; then
        echo $passwd | sudo -S umount /mnt/huge
    fi

    if [ -d /mnt/huge ] ; then
        echo $passwd | sudo -S rm -R /mnt/huge
    fi
}

#
# Removes all reserved hugepages.
#
clear_huge_pages()
{
    echo > .echo_tmp
    for d in /sys/devices/system/node/node? ; do
        echo "echo 0 > $d/hugepages/hugepages-${HUGEPGSZ}/nr_hugepages" >> .echo_tmp
    done
    echo "Removing currently reserved hugepages"
    echo $passwd | sudo -S sh .echo_tmp
    rm -f .echo_tmp

    remove_mnt_huge
}

#
# Creates hugepages on specific NUMA nodes.
#
set_numa_pages()
{
    clear_huge_pages

    echo > .echo_tmp
    for d in /sys/devices/system/node/node? ; do
        node=$(basename $d)
        echo "echo $Pages > $d/hugepages/hugepages-${HUGEPGSZ}/nr_hugepages" >> .echo_tmp
    done
    echo "Reserving hugepages"
    echo $passwd | sudo -S sh .echo_tmp
    rm -f .echo_tmp

    create_mnt_huge
}

#
# Uses dpdk-devbind.py to move devices to work with igb_uio
#
bind_devices_to_vfio_pci()
{
    echo "Bind device to DPDK"
    echo $passwd | sudo -S $RTE_SDK/usertools/dpdk-devbind.py -b vfio-pci $PCI_PATH
}

show_device() {
    $RTE_SDK/usertools/dpdk-devbind.py --status
}

setup_dpdk() {
    # insert Insert vfio_pci module
    load_vfio_pci_module

    # set hugepage mapping
    set_numa_pages

    # bind device
    bind_devices_to_vfio_pci
}

bind_dpdk() {
    bind_devices_to_vfio_pci
}

unbind_dpdk() {
    echo $passwd | sudo -S ${RTE_SDK}/usertools/dpdk-devbind.py -u $PCI_PATH
    echo $passwd | sudo -S ${RTE_SDK}/usertools/dpdk-devbind.py -b i40e $PCI_PATH
}

option="${1}"
case ${option} in
    show_device)
        show_device
        ;;
    setup_dpdk)
        setup_dpdk
        ;;
    bind_dpdk)
        bind_dpdk
        ;;
    unbind_dpdk)
        unbind_dpdk
        ;;
    *)  echo "usage:"
        echo "  ./tools.sh show_device: show device"
        echo "  ./tools.sh setup_dpdk: setup dpdk"
        echo "  ./tools.sh bind_dpdk: bind dpdk"
        echo "  ./tools.sh unbind_dpdk: unbind dpdk"
esac
