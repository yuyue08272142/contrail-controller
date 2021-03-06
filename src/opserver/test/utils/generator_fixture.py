#!/usr/bin/env python

#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

#
# generator_fixture.py
#
# Python generator test fixtures
#

from gevent import monkey
monkey.patch_all()
import fixtures
import socket
import uuid
import random
from util import retry
from pysandesh.sandesh_base import *
from pysandesh.util import UTCTimestampUsec
from sandesh.virtual_machine.ttypes import *
from sandesh.flow.ttypes import *
from analytics_fixture import AnalyticsFixture
from generator_introspect_utils import VerificationGenerator
from opserver_introspect_utils import VerificationOpsSrv


class GeneratorFixture(fixtures.Fixture):
    _BYTES_PER_PACKET = 1024
    _PKTS_PER_SEC = 100
    _INITIAL_PKT_COUNT = 20
    _VM_IF_PREFIX = 'vhost'
    _KSECINMSEC = 1000 * 1000

    def __init__(self, name, collector, logger,
                 opserver_port, start_time=None):
        self._name = name
        self._logger = logger
        self._collector = collector
        self._opserver_port = opserver_port
        self._start_time = start_time
    # end __init__

    def setUp(self):
        super(GeneratorFixture, self).setUp()
        self._sandesh_instance = Sandesh()
        self._http_port = AnalyticsFixture.get_free_port()
        self._sandesh_instance.init_generator(
            self._name, socket.gethostname(), [self._collector],
            '', self._http_port, sandesh_req_uve_pkg_list=['sandesh'])
        self._sandesh_instance.set_logging_params(enable_local_log=True,
                                                  level=SandeshLevel.UT_DEBUG)
    # end setUp

    @retry(delay=2, tries=5)
    def verify_on_setup(self):
        try:
            vg = VerificationGenerator('127.0.0.1', self._http_port)
            conn_status = vg.get_collector_connection_status()
        except:
            return False
        else:
            return conn_status['status'] == "Established"
    # end verify_on_setup

    def send_flow_stat(self, flow, flow_bytes, flow_pkts, ts=None):
        self._logger.info('Sending Flow Stats')
        if flow.bytes:
            flow.diff_bytes = flow_bytes - flow.bytes
        else:
            flow.diff_bytes = flow_bytes
        if flow.packets:
            flow.diff_packets = flow_pkts - flow.packets
        else:
            flow.diff_packets = flow_pkts
        flow.bytes = flow_bytes
        flow.packets = flow_pkts
        flow_data = FlowDataIpv4(
            flowuuid=flow.flowuuid, direction_ing=flow.direction_ing,
            sourcevn=flow.sourcevn, destvn=flow.destvn,
            sourceip=flow.sourceip, destip=flow.destip,
            dport=flow.dport, sport=flow.sport,
            protocol=flow.protocol, bytes=flow.bytes,
            packets=flow.packets, diff_bytes=flow.diff_bytes,
            diff_packets=flow.diff_packets)
        flow_object = FlowDataIpv4Object(flowdata=flow_data)
        # overwrite the timestamp of the flow, if specified.
        if ts:
            flow_object._timestamp = ts
        flow_object.send(sandesh=self._sandesh_instance)
    # end send_flow_stat

    def generate_flow_samples(self):
        self.flows = []
        self.flow_cnt = 5
        self.num_flow_samples = 0
        self.flow_start_time = None
        self.flow_end_time = None
        for i in range(self.flow_cnt):
            self.flows.append(FlowDataIpv4(flowuuid=str(uuid.uuid1()),
                                           direction_ing=1,
                                           sourcevn='domain1:admin:vn1',
                                           destvn='domain1:admin:vn2',
                                           sourceip=0x0A0A0A01,
                                           destip=0x0A0A0A02,
                                           sport=i + 10, dport=i + 100,
                                           protocol=i / 2))
            self._logger.info(str(self.flows[i]))

        # 'duration' - lifetime of the flow in seconds
        # 'tdiff'    - time difference between consecutive flow samples
        # 'pdiff'    - packet increment factor
        # 'psize'    - packet size
        flow_template = [
            {'duration': 60, 'tdiff':
             5, 'pdiff': 1, 'psize': 50},
            {'duration': 30, 'tdiff': 4,
             'pdiff': 2, 'psize': 100},
            {'duration': 20, 'tdiff':
             3, 'pdiff': 3, 'psize': 25},
            {'duration': 10, 'tdiff': 2,
             'pdiff': 4, 'psize': 75},
            {'duration': 5,  'tdiff':
             1, 'pdiff': 5, 'psize': 120}
        ]
        assert(len(flow_template) == self.flow_cnt)

        # set the flow_end_time to _start_time + (max duration in
        # flow_template)
        max_duration = 0
        for fd in flow_template:
            if max_duration < fd['duration']:
                max_duration = fd['duration']
        assert(self._start_time is not None)
        self.flow_start_time = self._start_time
        self.flow_end_time = self.flow_start_time + \
            (max_duration * self._KSECINMSEC)
        assert(self.flow_end_time <= UTCTimestampUsec())

        # generate flows based on the flow template defined above
        cnt = 0
        for fd in flow_template:
            num_samples = (fd['duration'] / fd['tdiff']) +\
                bool((fd['duration'] % fd['tdiff']))
            for i in range(num_samples):
                ts = self.flow_start_time + \
                    (i * fd['tdiff'] * self._KSECINMSEC) + \
                    random.randint(1, 10000)
                pkts = (i + 1) * fd['pdiff']
                bytes = pkts * fd['psize']
                self.num_flow_samples += 1
                self.send_flow_stat(self.flows[cnt], bytes, pkts, ts)
            cnt += 1
    # end generate_flow_samples

    def send_vm_uve(self, vm_id, num_vm_ifs, msg_count):
        vm_if_list = []
        vm_if_stats_list = []
        for num in range(num_vm_ifs):
            vm_if = VmInterfaceAgent()
            vm_if.name = self._VM_IF_PREFIX + str(num)
            vm_if_list.append(vm_if)
            vm_if_stats = VmInterfaceAgentStats()
            vm_if_stats.name = vm_if.name
            vm_if_stats.in_pkts = self._INITIAL_PKT_COUNT
            vm_if_stats.in_bytes = self._INITIAL_PKT_COUNT * \
                self._BYTES_PER_PACKET
            vm_if_stats_list.append(vm_if_stats)

        for num in range(msg_count):
            vm_agent = UveVirtualMachineAgent(interface_list=vm_if_list)
            vm_agent.name = vm_id
            if num != 0:
                for vm_if_stats in vm_if_stats_list:
                    vm_if_stats.in_pkts += self._PKTS_PER_SEC
                    vm_if_stats.in_bytes = vm_if_stats.in_pkts * \
                        self._BYTES_PER_PACKET
            vm_agent.if_stats_list = vm_if_stats_list
            uve_agent_vm = UveVirtualMachineAgentTrace(
                data=vm_agent,
                sandesh=self._sandesh_instance)
            uve_agent_vm.send(sandesh=self._sandesh_instance)
            self._logger.info(
                'Sent UveVirtualMachineAgentTrace:%s .. %d' % (vm_id, num))
    # end send_uve_vm

    @retry(delay=2, tries=5)
    def verify_vm_uve(self, vm_id, num_vm_ifs, msg_count):
        vns = VerificationOpsSrv('127.0.0.1', self._opserver_port)
        res = vns.get_ops_vm(vm_id)
        if res == {}:
            return False
        else:
            assert(len(res) > 0)
            self._logger.info(str(res))
            anum_vm_ifs = len(res.get_attr('Agent', 'interface_list'))
            assert anum_vm_ifs == num_vm_ifs
            anum_vm_if_stats = len(res.get_attr('Agent', 'if_stats_list'))
            assert anum_vm_if_stats == num_vm_ifs
            for i in range(num_vm_ifs):
                vm_if_dict = res.get_attr('Agent', 'interface_list')[i]
                vm_if_stats_dict = res.get_attr('Agent', 'if_stats_list')[i]
                evm_if_name = self._VM_IF_PREFIX + str(i)
                avm_if_name = vm_if_dict['name']
                assert avm_if_name == evm_if_name
                avm_if_stats_name = vm_if_stats_dict['name']
                assert avm_if_stats_name == evm_if_name
                epkt_count = self._INITIAL_PKT_COUNT + \
                    (msg_count - 1) * self._PKTS_PER_SEC
                apkt_count = vm_if_stats_dict['in_pkts']
                assert int(apkt_count) == epkt_count
                ebyte_count = epkt_count * self._BYTES_PER_PACKET
                abyte_count = vm_if_stats_dict['in_bytes']
                assert int(abyte_count) == ebyte_count
            return True
    # end verify_uve_vm
# end class GeneratorFixture
