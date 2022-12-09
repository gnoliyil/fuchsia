// automatically generated by stateify.

package udp

import (
	"gvisor.dev/gvisor/pkg/state"
)

func (p *udpPacket) StateTypeName() string {
	return "pkg/tcpip/transport/udp.udpPacket"
}

func (p *udpPacket) StateFields() []string {
	return []string{
		"udpPacketEntry",
		"netProto",
		"senderAddress",
		"destinationAddress",
		"packetInfo",
		"pkt",
		"receivedAt",
		"tosOrTClass",
		"ttlOrHopLimit",
	}
}

func (p *udpPacket) beforeSave() {}

// +checklocksignore
func (p *udpPacket) StateSave(stateSinkObject state.Sink) {
	p.beforeSave()
	var receivedAtValue int64
	receivedAtValue = p.saveReceivedAt()
	stateSinkObject.SaveValue(6, receivedAtValue)
	stateSinkObject.Save(0, &p.udpPacketEntry)
	stateSinkObject.Save(1, &p.netProto)
	stateSinkObject.Save(2, &p.senderAddress)
	stateSinkObject.Save(3, &p.destinationAddress)
	stateSinkObject.Save(4, &p.packetInfo)
	stateSinkObject.Save(5, &p.pkt)
	stateSinkObject.Save(7, &p.tosOrTClass)
	stateSinkObject.Save(8, &p.ttlOrHopLimit)
}

func (p *udpPacket) afterLoad() {}

// +checklocksignore
func (p *udpPacket) StateLoad(stateSourceObject state.Source) {
	stateSourceObject.Load(0, &p.udpPacketEntry)
	stateSourceObject.Load(1, &p.netProto)
	stateSourceObject.Load(2, &p.senderAddress)
	stateSourceObject.Load(3, &p.destinationAddress)
	stateSourceObject.Load(4, &p.packetInfo)
	stateSourceObject.Load(5, &p.pkt)
	stateSourceObject.Load(7, &p.tosOrTClass)
	stateSourceObject.Load(8, &p.ttlOrHopLimit)
	stateSourceObject.LoadValue(6, new(int64), func(y any) { p.loadReceivedAt(y.(int64)) })
}

func (e *endpoint) StateTypeName() string {
	return "pkg/tcpip/transport/udp.endpoint"
}

func (e *endpoint) StateFields() []string {
	return []string{
		"DefaultSocketOptionsHandler",
		"waiterQueue",
		"uniqueID",
		"net",
		"stats",
		"ops",
		"rcvReady",
		"rcvList",
		"rcvBufSize",
		"rcvClosed",
		"lastError",
		"portFlags",
		"boundBindToDevice",
		"boundPortFlags",
		"readShutdown",
		"effectiveNetProtos",
		"frozen",
		"localPort",
		"remotePort",
	}
}

// +checklocksignore
func (e *endpoint) StateSave(stateSinkObject state.Sink) {
	e.beforeSave()
	stateSinkObject.Save(0, &e.DefaultSocketOptionsHandler)
	stateSinkObject.Save(1, &e.waiterQueue)
	stateSinkObject.Save(2, &e.uniqueID)
	stateSinkObject.Save(3, &e.net)
	stateSinkObject.Save(4, &e.stats)
	stateSinkObject.Save(5, &e.ops)
	stateSinkObject.Save(6, &e.rcvReady)
	stateSinkObject.Save(7, &e.rcvList)
	stateSinkObject.Save(8, &e.rcvBufSize)
	stateSinkObject.Save(9, &e.rcvClosed)
	stateSinkObject.Save(10, &e.lastError)
	stateSinkObject.Save(11, &e.portFlags)
	stateSinkObject.Save(12, &e.boundBindToDevice)
	stateSinkObject.Save(13, &e.boundPortFlags)
	stateSinkObject.Save(14, &e.readShutdown)
	stateSinkObject.Save(15, &e.effectiveNetProtos)
	stateSinkObject.Save(16, &e.frozen)
	stateSinkObject.Save(17, &e.localPort)
	stateSinkObject.Save(18, &e.remotePort)
}

// +checklocksignore
func (e *endpoint) StateLoad(stateSourceObject state.Source) {
	stateSourceObject.Load(0, &e.DefaultSocketOptionsHandler)
	stateSourceObject.Load(1, &e.waiterQueue)
	stateSourceObject.Load(2, &e.uniqueID)
	stateSourceObject.Load(3, &e.net)
	stateSourceObject.Load(4, &e.stats)
	stateSourceObject.Load(5, &e.ops)
	stateSourceObject.Load(6, &e.rcvReady)
	stateSourceObject.Load(7, &e.rcvList)
	stateSourceObject.Load(8, &e.rcvBufSize)
	stateSourceObject.Load(9, &e.rcvClosed)
	stateSourceObject.Load(10, &e.lastError)
	stateSourceObject.Load(11, &e.portFlags)
	stateSourceObject.Load(12, &e.boundBindToDevice)
	stateSourceObject.Load(13, &e.boundPortFlags)
	stateSourceObject.Load(14, &e.readShutdown)
	stateSourceObject.Load(15, &e.effectiveNetProtos)
	stateSourceObject.Load(16, &e.frozen)
	stateSourceObject.Load(17, &e.localPort)
	stateSourceObject.Load(18, &e.remotePort)
	stateSourceObject.AfterLoad(e.afterLoad)
}

func (l *udpPacketList) StateTypeName() string {
	return "pkg/tcpip/transport/udp.udpPacketList"
}

func (l *udpPacketList) StateFields() []string {
	return []string{
		"head",
		"tail",
	}
}

func (l *udpPacketList) beforeSave() {}

// +checklocksignore
func (l *udpPacketList) StateSave(stateSinkObject state.Sink) {
	l.beforeSave()
	stateSinkObject.Save(0, &l.head)
	stateSinkObject.Save(1, &l.tail)
}

func (l *udpPacketList) afterLoad() {}

// +checklocksignore
func (l *udpPacketList) StateLoad(stateSourceObject state.Source) {
	stateSourceObject.Load(0, &l.head)
	stateSourceObject.Load(1, &l.tail)
}

func (e *udpPacketEntry) StateTypeName() string {
	return "pkg/tcpip/transport/udp.udpPacketEntry"
}

func (e *udpPacketEntry) StateFields() []string {
	return []string{
		"next",
		"prev",
	}
}

func (e *udpPacketEntry) beforeSave() {}

// +checklocksignore
func (e *udpPacketEntry) StateSave(stateSinkObject state.Sink) {
	e.beforeSave()
	stateSinkObject.Save(0, &e.next)
	stateSinkObject.Save(1, &e.prev)
}

func (e *udpPacketEntry) afterLoad() {}

// +checklocksignore
func (e *udpPacketEntry) StateLoad(stateSourceObject state.Source) {
	stateSourceObject.Load(0, &e.next)
	stateSourceObject.Load(1, &e.prev)
}

func init() {
	state.Register((*udpPacket)(nil))
	state.Register((*endpoint)(nil))
	state.Register((*udpPacketList)(nil))
	state.Register((*udpPacketEntry)(nil))
}
