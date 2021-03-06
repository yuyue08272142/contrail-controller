/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __ctrlplane__xmpp_factory__
#define __ctrlplane__xmpp_factory__

#include "base/factory.h"

class TcpServer;
class XmppChannelConfig;
class XmppChannelMux;
class XmppConnection;
class XmppClientConnection;
class XmppServerConnection;

class XmppObjectFactory : public Factory<XmppObjectFactory> {
    FACTORY_TYPE_N2(XmppObjectFactory, XmppServerConnection,
                    TcpServer *, const XmppChannelConfig *);
    FACTORY_TYPE_N2(XmppObjectFactory, XmppClientConnection,
                    TcpServer *, const XmppChannelConfig *);
    FACTORY_TYPE_N1(XmppObjectFactory, XmppChannelMux, XmppConnection *);
};

#endif /* defined(__ctrlplane__xmpp_factory__) */
