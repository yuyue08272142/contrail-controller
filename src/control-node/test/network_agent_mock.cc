/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "network_agent_mock.h"

#include <boost/algorithm/string.hpp>

#include "base/logging.h"
#include "base/util.h"
#include "base/task_annotations.h"
#include "base/test/task_test_util.h"
#include "bgp/ipeer.h"
#include "bgp/bgp_xmpp_channel.h"
#include "net/bgp_af.h"
#include "schema/xmpp_unicast_types.h"
#include "schema/xmpp_multicast_types.h"
#include "schema/vnc_cfg_types.h"
#include "schema/xmpp_enet_types.h"
#include "xml/xml_pugi.h"
#include "xmpp/xmpp_connection.h"
#include "xmpp/xmpp_client.h"
#include "xmpp/xmpp_factory.h"

using namespace std;
using namespace pugi;
using boost::asio::ip::address;

namespace test {

const char *XmppDocumentMock::kControlNodeJID =
    "network-control@contrailsystems.com";
const char *XmppDocumentMock::kNetworkServiceJID =
    "network-control@contrailsystems.com/bgp-peer";
const char *XmppDocumentMock::kConfigurationServiceJID =
    "network-control@contrailsystems.com/config";
const char *XmppDocumentMock::kPubSubNS =
    "http://jabber.org/protocol/pubsub";

class NetworkAgentMock::AgentPeer : public BgpXmppChannel {
public:
    AgentPeer(NetworkAgentMock *parent, XmppChannel *channel)
        : BgpXmppChannel(channel, NULL, NULL), parent_(parent) {
        channel->RegisterReceive(xmps::CONFIG,
            boost::bind(&NetworkAgentMock::AgentPeer::ReceiveConfigUpdate,
                        this, _1));
    }
    virtual ~AgentPeer() {
        set_deleted(true);
        Close();
    }

    virtual void ReceiveConfigUpdate(const XmppStanza::XmppMessage *msg) {
        if (parent_->down()) return;
        tbb::mutex::scoped_lock lock(parent_->get_mutex());
        if (parent_->down()) return;

        XmlPugi *pugi = static_cast<XmlPugi *>(msg->dom.get());
        xml_node config = pugi->FindNode("config");

        for (xml_node node = config.first_child(); node;
            node = node.next_sibling()) {

            if (strcmp(node.name(), "update") == 0) {
            } else if (strcmp(node.name(), "delete") == 0) { 
            } else {
                continue;
            }

            for(xml_node child = node.first_child(); child;
                    child = child.next_sibling()) {
    
                // Handle the links between the nodes
                if (strcmp(child.name(), "link") == 0) {
                    // LinkParse(child, oper, seq);
                    continue;
                }

                if (strcmp(child.name(), "node") == 0) {
                    const char *name = child.attribute("type").value();

                    if (!strcmp(name, "virtual-router")) {
                        string id_name = "virtual-router";
                        autogen::VirtualRouter *data = new autogen::VirtualRouter();
                        assert(autogen::VirtualRouter::Decode(child, &id_name,
                                                              data));
                        id_name = "virtual-router:" + id_name;
                        parent_->vrouter_mgr_->Update(id_name, id_name, data);
                    }

                    if (!strcmp(name, "virtual-machine")) {
                        string id_name = "virtual-machine";
                        autogen::VirtualMachine *data = new autogen::VirtualMachine();
                        assert(autogen::VirtualMachine::Decode(child, &id_name,
                                                               data));
                        id_name = "virtual-machine:" + id_name;
                        parent_->vm_mgr_->Update(id_name, id_name, data);
                    }
                }
            }        
        }
    }

    virtual void ReceiveUpdate(const XmppStanza::XmppMessage *msg) {
        if (parent_->down()) return;
        tbb::mutex::scoped_lock lock(parent_->get_mutex());
        if (parent_->down()) return;

        XmlPugi *pugi = static_cast<XmlPugi *>(msg->dom.get());
        xml_node items = pugi->FindNode("items");
        xml_attribute node = items.attribute("node");

        std::string nodename(node.value());
        bool inet_route = false;
        const char *af = NULL, *safi = NULL, *network;
        char *str = const_cast<char *>(nodename.c_str());
        char *saveptr;
        af = strtok_r(str, "/", &saveptr);
        safi = strtok_r(NULL, "/", &saveptr);
        network = saveptr;

        if (atoi(af) == BgpAf::IPv4 && atoi(safi) == BgpAf::Unicast) {
            inet_route = true;
        }

        xml_node retract_node = pugi->FindNode("retract");
        if (!pugi->IsNull(retract_node)) {
            while (retract_node) {
                std::string sid = retract_node.first_attribute().value();
                if (inet_route) {
                    if (parent_->skip_updates_processing()) {
                        parent_->route_mgr_->Update(node.value(), -1);
                    } else {
                        parent_->route_mgr_->Remove(network, sid);
                    }
                } else {
                    if (atoi(af) == BgpAf::L2Vpn && atoi(safi) == BgpAf::Enet) {
                        if (parent_->skip_updates_processing()) {
                            parent_->enet_route_mgr_->Update(network, -1);
                        } else {
                            parent_->enet_route_mgr_->Remove(network, sid);
                        }
                    }
                }
                retract_node = retract_node.next_sibling();
            }
        } else {
            for (xml_node item = items.first_child(); item;
                 item = item.next_sibling()) {
                xml_attribute id = item.attribute("id");
                std::string sid = id.value();
                if (inet_route) {
                    autogen::ItemType *rt_entry = new autogen::ItemType();
                    if (!rt_entry->XmlParse(item)) {
                        delete rt_entry;
                        continue;
                    }

                    if (parent_->skip_updates_processing()) {
                        parent_->route_mgr_->Update(network, +1);
                    } else {
                        parent_->route_mgr_->Update(network, id.value(), rt_entry);
                    }
                } else {
                    if (atoi(af) == BgpAf::L2Vpn && atoi(safi) == BgpAf::Enet) {
                        autogen::EnetItemType *rt_entry =
                            new autogen::EnetItemType();
                        if (!rt_entry->XmlParse(item)) {
                            delete rt_entry;
                            continue;
                        }
                        if (parent_->skip_updates_processing()) {
                            parent_->enet_route_mgr_->Update(network, -1);
                        } else {
                            parent_->enet_route_mgr_->Update(
                                network, id.value(), rt_entry);
                        }
                    }
                }
            }
        }
    }

    void SendDocument(const pugi::xml_document *xdoc) {
        ostringstream oss;
        xdoc->save(oss);
        string msg = oss.str();
        boost::erase_all(msg, "\n");
        boost::erase_all(msg, "\t");
        Peer()->SendUpdate(reinterpret_cast<const uint8_t *>(msg.data()),
                           msg.length());
    }

private:
    NetworkAgentMock *parent_;
};

XmppDocumentMock::XmppDocumentMock(const std::string &hostname) 
    : hostname_(hostname), label_alloc_(10000), xdoc_(new pugi::xml_document) {
        localaddr_ = "127.0.0.1";
} 

pugi::xml_document *XmppDocumentMock::RouteAddXmlDoc(
        const std::string &network, const std::string &prefix, 
        const std::string nexthop) {
    return RouteAddDeleteXmlDoc(network, prefix, true, nexthop);
}

pugi::xml_document *XmppDocumentMock::RouteDeleteXmlDoc(
        const std::string &network, const std::string &prefix,
        const std::string nexthop) {
    return RouteAddDeleteXmlDoc(network, prefix, false, nexthop);
}

pugi::xml_document *XmppDocumentMock::RouteEnetAddXmlDoc(
        const std::string &network, const std::string &prefix,
        const std::string nexthop) {
    return RouteEnetAddDeleteXmlDoc(network, prefix, nexthop, true);
}

pugi::xml_document *XmppDocumentMock::RouteEnetDeleteXmlDoc(
        const std::string &network, const std::string &prefix,
        const std::string nexthop) {
    return RouteEnetAddDeleteXmlDoc(network, prefix, nexthop, false);
}

pugi::xml_document *XmppDocumentMock::RouteMcastAddXmlDoc(
        const std::string &network, const std::string &sg,
        const std::string &nexthop,
        const std::string &label_range) {
    return RouteMcastAddDeleteXmlDoc(network, sg, nexthop, label_range, true);
}

pugi::xml_document *XmppDocumentMock::RouteMcastDeleteXmlDoc(
        const std::string &network, const std::string &sg) {
    return RouteMcastAddDeleteXmlDoc(network, sg, "", "", false);
}


pugi::xml_document *XmppDocumentMock::SubscribeXmlDoc(
        const std::string &network, int id, string type) {
    return SubUnsubXmlDoc(network, id, true, type);
}

pugi::xml_document *XmppDocumentMock::UnsubscribeXmlDoc(
        const std::string &network, int id, string type) {
    return SubUnsubXmlDoc(network, id, false, type);
}

xml_node XmppDocumentMock::PubSubHeader(string type) {
    xml_node iq = xdoc_->append_child("iq");
    iq.append_attribute("type") = "set";
    iq.append_attribute("from") = hostname().c_str();
    iq.append_attribute("to") = type.c_str();
    // TODO: iq.append_attribute("id") =
    xml_node pubsub = iq.append_child("pubsub");
    pubsub.append_attribute("xmlns") = kPubSubNS; 
    return pubsub;
}

pugi::xml_document *XmppDocumentMock::SubUnsubXmlDoc(
        const std::string &network, int id, bool sub, string type) {
    xdoc_->reset();
    xml_node pubsub = PubSubHeader(type);
    xml_node subscribe = pubsub.append_child(
            sub ? "subscribe" : "unsubscribe" );
    subscribe.append_attribute("node") = network.c_str();
    if (id >= 0) {
        xml_node options = pubsub.append_child("options");
        xml_node instance_id = options.append_child("instance-id");
        instance_id.text().set(id);
    }
    return xdoc_.get();
}

pugi::xml_document *XmppDocumentMock::RouteAddDeleteXmlDoc(
        const std::string &network, const std::string &prefix, bool add,
        const std::string nexthop) {
    xdoc_->reset();
    xml_node pubsub = PubSubHeader(kNetworkServiceJID);
    xml_node pub = pubsub.append_child("publish");
    stringstream header;
    header << BgpAf::IPv4 << "/" <<  BgpAf::Unicast << "/" <<
              network.c_str() << "/" << prefix.c_str();
    pub.append_attribute("node") = header.str().c_str();
    autogen::ItemType rt_entry;
    rt_entry.Clear();
    rt_entry.entry.nlri.af = BgpAf::IPv4;
    rt_entry.entry.nlri.safi = BgpAf::Unicast;
    rt_entry.entry.nlri.address = prefix;
    rt_entry.entry.security_group_list.security_group.push_back(101);

    autogen::NextHopType item_nexthop;

    item_nexthop.af = BgpAf::IPv4;
    item_nexthop.safi = BgpAf::Unicast;
    item_nexthop.address = !nexthop.empty() ? nexthop : localaddr();
    item_nexthop.label = add ? label_alloc_++ : 0xFFFFF;
    item_nexthop.tunnel_encapsulation_list.tunnel_encapsulation.push_back(std::string("gre"));
    rt_entry.entry.next_hops.next_hop.push_back(item_nexthop);

    xml_node item = pub.append_child("item");
    rt_entry.Encode(&item);
    pubsub = PubSubHeader(kNetworkServiceJID);
    xml_node collection = pubsub.append_child("collection");
    collection.append_attribute("node") = network.c_str();
    xml_node assoc = collection.append_child(
            add ? "associate" : "dissociate");
    assoc.append_attribute("node") = header.str().c_str();
    return xdoc_.get();
}

pugi::xml_document *XmppDocumentMock::RouteEnetAddDeleteXmlDoc(
        const std::string &network, const std::string &prefix,
        const std::string nexthop, bool add) {
    xdoc_->reset();
    xml_node pubsub = PubSubHeader(kNetworkServiceJID);
    xml_node pub = pubsub.append_child("publish");
    stringstream node_str;
    node_str << BgpAf::L2Vpn << "/" << BgpAf::Enet << "/" 
             << network << "/" << prefix;
    pub.append_attribute("node") = node_str.str().c_str();
    autogen::EnetItemType rt_entry;
    rt_entry.Clear();

    std::string temp(prefix.c_str());
    char *str = const_cast<char *>(temp.c_str());
    char *saveptr;
    char *mac = strtok_r(str, ",", &saveptr);
    char *address = strtok_r(NULL, "", &saveptr);

    rt_entry.entry.nlri.af = BgpAf::L2Vpn;
    rt_entry.entry.nlri.safi = BgpAf::Enet;
    rt_entry.entry.nlri.mac = std::string(mac) ;
    rt_entry.entry.nlri.address = std::string(address);

    autogen::EnetNextHopType item_nexthop;

    item_nexthop.af = BgpAf::IPv4;
    item_nexthop.address = !nexthop.empty() ? nexthop : localaddr();
    item_nexthop.label = add ? label_alloc_++ : 0xFFFFF;
    rt_entry.entry.next_hops.next_hop.push_back(item_nexthop);

    xml_node item = pub.append_child("item");
    rt_entry.Encode(&item);
    pubsub = PubSubHeader(kNetworkServiceJID);
    xml_node collection = pubsub.append_child("collection");
    collection.append_attribute("node") = network.c_str();
    xml_node assoc = collection.append_child(
            add ? "associate" : "dissociate");
    assoc.append_attribute("node") = node_str.str().c_str();
    return xdoc_.get();
}

pugi::xml_document *XmppDocumentMock::RouteMcastAddDeleteXmlDoc(
        const std::string &network, const std::string &sg, 
        const std::string &nexthop,
        const std::string &lrange, bool add) {
    xdoc_->reset();
    string sg_save(sg.c_str());
    xml_node pubsub = PubSubHeader(kNetworkServiceJID);
    xml_node pub = pubsub.append_child("publish");
    stringstream node_str;
    node_str << BgpAf::IPv4 << "/" << BgpAf::Mcast << "/" 
             << network << "/" << sg_save;
    pub.append_attribute("node") = node_str.str().c_str();
    autogen::McastItemType rt_entry;
    rt_entry.Clear();

    char *str = const_cast<char *>(sg.c_str());
    char *saveptr;
    char *group = strtok_r(str, ",", &saveptr);
    char *source = NULL;
    if (group == NULL) {
        group = strtok_r(NULL, "", &saveptr);
    } else {
        source = strtok_r(NULL, "", &saveptr);
    }

    rt_entry.entry.nlri.af = BgpAf::IPv4;
    rt_entry.entry.nlri.safi = BgpAf::Mcast;
    rt_entry.entry.nlri.group = std::string(group) ;
    if (source != NULL) {
        rt_entry.entry.nlri.source = std::string(source);
    } else {
        rt_entry.entry.nlri.source = std::string("0.0.0.0");
    }

    autogen::McastNextHopType item_nexthop;

    if (!nexthop.empty()) {
        item_nexthop.af = BgpAf::IPv4;
        item_nexthop.safi = BgpAf::Mcast;
        item_nexthop.address = nexthop.c_str();
        if (!lrange.empty()) {
            item_nexthop.label = lrange;
        }
        rt_entry.entry.next_hops.next_hop.push_back(item_nexthop);
    }

    xml_node item = pub.append_child("item");
    rt_entry.Encode(&item);
    pubsub = PubSubHeader(kNetworkServiceJID);
    xml_node collection = pubsub.append_child("collection");
    collection.append_attribute("node") = network.c_str();
    xml_node assoc = collection.append_child(
            add ? "associate" : "dissociate");
    assoc.append_attribute("node") = node_str.str().c_str();
    return xdoc_.get();
}

// XmppChannelMuxTest mock test class to protect rxmap_ from parallel accesses
// between main and XmppStateMachine tasks.
class XmppChannelMuxTest : public XmppChannelMux {
public:
    XmppChannelMuxTest(XmppConnection *conn) : XmppChannelMux(conn) { }

    // Protext XmppChannelMux::rxmap_ from parallel threads, as it directly
    // used off main() by NetworkAgentMock clients. Production code always
    // accesses them off xmpp::XmppStateMachine task or bgp::Config task, which
    // are mutually exclusive.
    void RegisterReceive(xmps::PeerId id, ReceiveCb cb) {
        tbb::mutex::scoped_lock lock(mutex_);
        XmppChannelMux::RegisterReceive(id, cb);
    }
    void UnRegisterReceive(xmps::PeerId id) {
        tbb::mutex::scoped_lock lock(mutex_);
        XmppChannelMux::UnRegisterReceive(id);
    }
    void ProcessXmppMessage(const XmppStanza::XmppMessage *msg) {
        tbb::mutex::scoped_lock lock(mutex_);
        XmppChannelMux::ProcessXmppMessage(msg);
    }

private:
    tbb::mutex mutex_;
};

void NetworkAgentMock::Initialize() {
    static bool init_ = false;

    if (init_) return;
    init_ = true;

    XmppObjectFactory::Register<XmppChannelMux>(
        boost::factory<XmppChannelMuxTest *>());
}

NetworkAgentMock::NetworkAgentMock(EventManager *evm, const string &hostname,
                                   int server_port, string local_address,
                                   string server_address)
    : client_(new XmppClient(evm)), impl_(new XmppDocumentMock(hostname)),
      work_queue_(TaskScheduler::GetInstance()->GetTaskId("bgp::Config"), 0,
                boost::bind(&NetworkAgentMock::ProcessRequest, this, _1)),
      server_address_(server_address), local_address_(local_address),
      server_port_(server_port), skip_updates_processing_(false), down_(false) {

    // Static initialization of NetworkAgentMock class.
    Initialize();

    route_mgr_.reset(new InstanceMgr<RouteEntry>(this,
            XmppDocumentMock::kNetworkServiceJID));
    enet_route_mgr_.reset(new InstanceMgr<EnetRouteEntry>(this,
            XmppDocumentMock::kNetworkServiceJID));
    vrouter_mgr_.reset(new InstanceMgr<VRouterEntry>(this,
            XmppDocumentMock::kConfigurationServiceJID));
    vm_mgr_.reset(new InstanceMgr<VMEntry>(this,
            XmppDocumentMock::kConfigurationServiceJID));

    XmppConfigData *data = new XmppConfigData();
    data->AddXmppChannelConfig(CreateXmppConfig());
    client_->ConfigUpdate(data);
    if (!local_address.empty()) {
        impl_->set_localaddr(local_address);
    }
    down_ = false;
}

void NetworkAgentMock::DisableRead(bool disable_read) {
    XmppConnection *connection;

    connection = client_->FindConnection("network-control@contrailsystems.com");
    if (connection) connection->set_disable_read(disable_read);
}

const string NetworkAgentMock::ToString() const {
    ostringstream ostr;

    ostr << hostname() << " " << localaddr() << " > " << server_address_;
    return ostr.str();
}

void NetworkAgentMock::ClearInstances() {
    route_mgr_->Clear();
    enet_route_mgr_->Clear();
    vrouter_mgr_->Clear();
    vm_mgr_->Clear();
}

NetworkAgentMock::~NetworkAgentMock() {
    tbb::mutex::scoped_lock lock(mutex_);
    down_ = true;
    ClearInstances();

    AgentPeer *peer = peer_.get();
    peer_.release();

    if (peer) {
        delete peer;
    }
    assert(!client_);
}

void NetworkAgentMock::Delete() {
    AgentPeer *peer = peer_.get();
    peer_.release();
    if (peer) {
        delete peer;
    }
    client_->Shutdown();
    task_util::WaitForIdle();
    TcpServerManager::DeleteServer(client_);
    client_ = NULL;
}

XmppChannelConfig *NetworkAgentMock::CreateXmppConfig() {
    XmppChannelConfig *config = new XmppChannelConfig(true);
    config->endpoint.address(address::from_string(server_address_));

#ifdef __APPLE__

    //
    // XXX On darwin, for non 127.0.0.1, explicitly add the address to lo0
    //
    // e.g. sudo /sbin/ifconfig lo0 alias 127.0.0.2
    //
#endif

    config->local_endpoint.address(address::from_string(local_address_));
    config->local_endpoint.port(0);
    config->endpoint.port(server_port_);
    config->FromAddr = hostname();
    config->ToAddr = XmppDocumentMock::kControlNodeJID;
    return config;
}

NetworkAgentMock::AgentPeer *NetworkAgentMock::GetAgent() {
    AgentPeer *peer = peer_.get();
    if (peer == NULL) {
        XmppChannel *channel = client_->FindChannel(
                XmppDocumentMock::kControlNodeJID);
        assert(channel != NULL);
        peer = new AgentPeer(this, channel);
        peer_.reset(peer);
    }
    return peer;
}

void NetworkAgentMock::SessionDown() {
    tbb::mutex::scoped_lock lock(mutex_);
    down_ = true;
    ClearInstances();
    peer_.reset(NULL);
    XmppConfigData *data = new XmppConfigData();
    client_->ConfigUpdate(data);
}

void NetworkAgentMock::SessionUp() {
    tbb::mutex::scoped_lock lock(mutex_);
    XmppConfigData *data = new XmppConfigData();
    data->AddXmppChannelConfig(CreateXmppConfig());
    client_->ConfigUpdate(data);
    down_ = false;
}

//
// Process requests and run them off bgp::Config exclusive task
//
bool NetworkAgentMock::ProcessRequest(Request *request) {
    CHECK_CONCURRENCY("bgp::Config");
    switch (request->type) {
        case IS_ESTABLISHED:
            request->result = IsSessionEstablished();
            break;
    }

    //
    // Notify waiting caller with the result
    //
    tbb::mutex::scoped_lock lock(work_mutex_);
    cond_var_.notify_all();
    return true;
}

bool NetworkAgentMock::IsSessionEstablished() {
    XmppChannel *channel = client_->FindChannel(
            XmppDocumentMock::kControlNodeJID);
    return (channel != NULL && channel->GetPeerState() == xmps::READY);
}

bool NetworkAgentMock::IsEstablished() {
    std::unique_lock<tbb::mutex> lock(work_mutex_);

    Request request;
    request.type = IS_ESTABLISHED;
    work_queue_.Enqueue(&request);

    //
    // Wait for the request to get processed.
    //
    cond_var_.wait(lock);

    return request.result;
}

void NetworkAgentMock::AddRoute(const string &network_name,
                                const string &prefix, const string nexthop) {
    AgentPeer *peer = GetAgent();
    xml_document *xdoc = impl_->RouteAddXmlDoc(network_name, prefix, nexthop);

    peer->SendDocument(xdoc);
}

void NetworkAgentMock::DeleteRoute(const string &network_name,
                                   const string &prefix, const string nexthop) {
    AgentPeer *peer = GetAgent();
    xml_document *xdoc = impl_->RouteDeleteXmlDoc(network_name, prefix,
                                                  nexthop);
    peer->SendDocument(xdoc);
}

void NetworkAgentMock::AddEnetRoute(const string &network_name,
        const string &prefix, const string nexthop) {
    AgentPeer *peer = GetAgent();
    xml_document *xdoc =
        impl_->RouteEnetAddXmlDoc(network_name, prefix, nexthop);

    peer->SendDocument(xdoc);
}

void NetworkAgentMock::DeleteEnetRoute(const string &network_name,
        const string &prefix, const string nexthop) {
    AgentPeer *peer = GetAgent();
    xml_document *xdoc =
        impl_->RouteEnetDeleteXmlDoc(network_name, prefix, nexthop);
    peer->SendDocument(xdoc);
}

void NetworkAgentMock::AddMcastRoute(const string &network_name,
                                     const string &sg,
                                     const string &nexthop,
                                     const string &label_range) {
    AgentPeer *peer = GetAgent();
    xml_document *xdoc = impl_->RouteMcastAddXmlDoc(network_name, 
                                                    sg, nexthop, label_range);
    peer->SendDocument(xdoc);
}

void NetworkAgentMock::DeleteMcastRoute(const string &network_name,
                                        const string &sg) {
    AgentPeer *peer = GetAgent();
    xml_document *xdoc = impl_->RouteMcastDeleteXmlDoc(network_name, sg);
    peer->SendDocument(xdoc);
}

template<typename T>
NetworkAgentMock::Instance<T>::Instance() {
    count_ = 0;
}

template<typename T>
NetworkAgentMock::Instance<T>::~Instance() {
    STLDeleteElements(&table_);
}

template<typename T>
void NetworkAgentMock::Instance<T>::Update(long count) {
    count_ += count;
}

template<typename T>
void NetworkAgentMock::Instance<T>::Update(const std::string &node, T *entry) {
    count_++;

    //
    // If a route already exists, delete it
    //
    Remove(node);
    table_.insert(make_pair(node, entry));
}

template<typename T>
void NetworkAgentMock::Instance<T>::Remove(const std::string &node) {
    typename TableMap::iterator iter = table_.find(node);

    if (iter != table_.end()) {
        T *entry = iter->second;
        table_.erase(node);
        delete entry;
        count_--;
    }
}

template<typename T>
void NetworkAgentMock::Instance<T>::Clear() {
    count_ = 0;
    STLDeleteElements(&table_);
}

template<typename T>
int NetworkAgentMock::Instance<T>::Count() const {
    return count_;
    return table_.size();
}

template<typename T>
const T *NetworkAgentMock::Instance<T>::Lookup(const std::string &node) const {
    typename TableMap::const_iterator loc = table_.find(node);
    if (loc != table_.end()) {
        return loc->second;
    }
    return NULL;
}

template<typename T>
void NetworkAgentMock::InstanceMgr<T>::Subscribe(const std::string &network,
                                                 int id,
                                                 bool wait_for_established) {
    if (wait_for_established) {
        TASK_UTIL_EXPECT_EQ_MSG(true, parent_->IsEstablished(),
                                "Waiting for agent " << parent_->ToString() <<
                                " to become established");
    }
    tbb::mutex::scoped_lock lock(parent_->get_mutex());

    typename InstanceMap::iterator loc = instance_map_.find(network);
    if (loc == instance_map_.end()) {
        Instance<T> *rti = new Instance<T>();
        instance_map_.insert(make_pair(network, rti));
    }

    xml_document *xdoc;
    xdoc = parent_->GetXmlHandler()->SubscribeXmlDoc(network, id, type_);

    AgentPeer *peer = parent_->GetAgent();
    assert(peer != NULL);
    peer->SendDocument(xdoc);
}

template<typename T>
bool NetworkAgentMock::InstanceMgr<T>::HasSubscribed(const std::string &network) {
    // if (!parent_->IsEstablished()) return false;

    tbb::mutex::scoped_lock lock(parent_->get_mutex());

    typename InstanceMap::iterator loc = instance_map_.find(network);
    return (loc != instance_map_.end());
}

template<typename T>
void NetworkAgentMock::InstanceMgr<T>::Unsubscribe(const std::string &network,
                                                   int id,
                                                   bool wait_for_established) {
    if (wait_for_established) {
        TASK_UTIL_EXPECT_EQ_MSG(true, parent_->IsEstablished(),
                                "Waiting for agent " << parent_->ToString() <<
                                " to become established");
    }

    tbb::mutex::scoped_lock lock(parent_->get_mutex());

    pugi::xml_document *xdoc;
    xdoc = parent_->GetXmlHandler()->UnsubscribeXmlDoc(network, id, type_);

    AgentPeer *peer = parent_->GetAgent();
    assert(peer != NULL);
    peer->SendDocument(xdoc);

    //
    // Delete all entries locally, as we do not get an route
    // retract messages as a result of unsubscribe
    //
    Instance<T> *rti;
    typename InstanceMap::iterator loc = instance_map_.find(network);
    if (loc == instance_map_.end()) return;

    rti = loc->second;
    rti->Clear();

    //
    // Remvoe the table from the map
    //
    instance_map_.erase(loc);
    delete rti;
}

template<typename T>
void NetworkAgentMock::InstanceMgr<T>::Update(const std::string &network,
                                              long count) {
    Instance<T> *rti;
    typename InstanceMgr<T>::InstanceMap::iterator loc;

    loc = instance_map_.find(network);

    if (loc != instance_map_.end()) {
        rti = loc->second;
        rti->Update(count);
    }
}

template<typename T>
void NetworkAgentMock::InstanceMgr<T>::Update(const std::string &network,
                                              const std::string &node_name,
                                              T *rt_entry) {
    Instance<T> *rti;
    typename InstanceMgr<T>::InstanceMap::iterator loc;

    loc = instance_map_.find(network);

    //
    // Ignore updates for instances to which the agent has not subscribed
    //
    if (loc == instance_map_.end()) {
        delete rt_entry;
        return;
    }

    rti = loc->second;
    rti->Update(node_name, rt_entry);
}

template<typename T>
void NetworkAgentMock::InstanceMgr<T>::Remove(const std::string &network,
        const std::string &node_name) {
    Instance<T> *rti;
    typename InstanceMgr<T>::InstanceMap::iterator loc;

    loc = instance_map_.find(network);

    if (loc == instance_map_.end()) return;

    rti = loc->second;
    rti->Remove(node_name);
}

template<typename T>
int NetworkAgentMock::InstanceMgr<T>::Count(const std::string &network) const {
    typename InstanceMgr<T>::InstanceMap::const_iterator loc;

    tbb::mutex::scoped_lock lock(parent_->get_mutex());
    loc = instance_map_.find(network);
    if (loc == instance_map_.end()) {
        return 0;
    }
    return loc->second->Count();
}

template<typename T>
int NetworkAgentMock::InstanceMgr<T>::Count() const {
    int count = 0;

    tbb::mutex::scoped_lock lock(parent_->get_mutex());
    for (typename InstanceMgr<T>::InstanceMap::const_iterator iter = instance_map_.begin();
            iter != instance_map_.end(); ++iter) {
        count += iter->second->Count();
    }

    return count;
}

template<typename T>
void NetworkAgentMock::InstanceMgr<T>::Clear() {
    STLDeleteElements(&instance_map_);
}

template<typename T>
const T *NetworkAgentMock::InstanceMgr<T>::Lookup(const std::string &network,
        const std::string &prefix) const {
    typename InstanceMgr<T>::InstanceMap::const_iterator loc;
    tbb::mutex::scoped_lock lock(parent_->get_mutex());

    loc = instance_map_.find(network);
    if (loc == instance_map_.end()) {
        return NULL;
    }
    return loc->second->Lookup(prefix);
}

#define INSTANTIATE_INSTANCE_TEMPLATES(T) \
template NetworkAgentMock::Instance<T>::Instance(); \
template NetworkAgentMock::Instance<T>::~Instance(); \
template void NetworkAgentMock::Instance<T>::Update(long count); \
template void NetworkAgentMock::Instance<T>::Update(const std::string &node, \
                                                    T *entry); \
template void NetworkAgentMock::Instance<T>::Remove(const std::string &node); \
template void NetworkAgentMock::Instance<T>::Clear(); \
template int NetworkAgentMock::Instance<T>::Count() const; \
template const T *NetworkAgentMock::Instance<T>::Lookup(const std::string &node) const; \
 \
template void NetworkAgentMock::InstanceMgr<T>::Subscribe(const std::string &network, int id, bool wait_for_established); \
template bool NetworkAgentMock::InstanceMgr<T>::HasSubscribed(const std::string &network); \
template void NetworkAgentMock::InstanceMgr<T>::Unsubscribe(const std::string &network, int id, bool wait_for_established); \
template void NetworkAgentMock::InstanceMgr<T>::Update(const std::string &network, long count); \
template void NetworkAgentMock::InstanceMgr<T>::Remove(const std::string &network, const std::string &node_name); \
template int NetworkAgentMock::InstanceMgr<T>::Count(const std::string &network) const; \
template int NetworkAgentMock::InstanceMgr<T>::Count() const; \
template void NetworkAgentMock::InstanceMgr<T>::Clear(); \
template const T *NetworkAgentMock::InstanceMgr<T>::Lookup(const std::string &network, const std::string &prefix) const;

INSTANTIATE_INSTANCE_TEMPLATES(NetworkAgentMock::RouteEntry)
INSTANTIATE_INSTANCE_TEMPLATES(NetworkAgentMock::EnetRouteEntry)
INSTANTIATE_INSTANCE_TEMPLATES(NetworkAgentMock::VRouterEntry)
INSTANTIATE_INSTANCE_TEMPLATES(NetworkAgentMock::VMEntry)

int NetworkAgentMock::RouteCount(const std::string &network) const {
    return route_mgr_->Count(network);
}

int NetworkAgentMock::RouteCount() const {
    return route_mgr_->Count();
}

int NetworkAgentMock::EnetRouteCount(const std::string &network) const {
    return enet_route_mgr_->Count(network);
}

int NetworkAgentMock::EnetRouteCount() const {
    return enet_route_mgr_->Count();
}

} // namespace test
