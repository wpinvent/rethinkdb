#include "rpc/core/cluster.hpp"
#include "arch/arch.hpp"
#include "utils.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include "concurrency/mutex.hpp"
#include "protob.hpp"
#include <string>
#include "logger.hpp"
#include "rpc/core/pop_srvc.hpp"
#include "rpc/core/mbox_srvc.hpp"
#include "rpc/core/mailbox.pb.h"

cluster_mailbox_t::cluster_mailbox_t() {
    get_cluster()->add_mailbox(this);
}

cluster_mailbox_t::~cluster_mailbox_t() {
    get_cluster()->remove_mailbox(this);
}

/* Concrete subclass of `checking_outpipe_t` that writes to a TCP connection. */

struct cluster_peer_outpipe_t : public checking_outpipe_t {
    void do_write(const void *buf, size_t size) {
        try {
            conn->write(buf, size);
        } catch (tcp_conn_t::write_closed_exc_t) {}
    }
    cluster_peer_outpipe_t(tcp_conn_t *conn, int bytes) : checking_outpipe_t(bytes), conn(conn) {}
    tcp_conn_t *conn;
};

/* Establishing a cluster */

static cluster_t *the_cluster = NULL;

cluster_t *get_cluster() {
    return the_cluster;
}

cluster_t::cluster_t(int port, cluster_delegate_t *d) :
    delegate(d), listener(new tcp_listener_t(port, boost::bind(&cluster_t::on_tcp_listener_accept, this, _1)))
{
    rassert(the_cluster == NULL);
    the_cluster = this;

    /* Initially there is only one node in the cluster: us */
    us = 0;
    peers[0] = boost::make_shared<cluster_peer_t>(port, 0);
    print_peers();
}

cluster_t::cluster_t(int port, const char *contact_host, int contact_port,
                     boost::function<cluster_delegate_t *(cluster_inpipe_t *, boost::function<void()>)> startup_function) :
    listener(new tcp_listener_t(port, boost::bind(&cluster_t::on_tcp_listener_accept, this, _1)))
{
    rassert(the_cluster == NULL);
    the_cluster = this;

    population::Join_initial initial;
    population::Join_welcome welcome;

    /* Get in touch with our specified contact */
    tcp_conn_t contact_conn(contact_host, contact_port);

    /* send a join request to to the cluster */
    initial.mutable_addr()->set_ip(ip_address_t::us().ip_as_uint32());
    initial.mutable_addr()->set_port(port);
    initial.mutable_addr()->set_id(-1); //we don't know our id

    write_protob(&contact_conn, &initial);

    /* receive a welcome packet off the socket */
    read_protob(&contact_conn, &welcome);

    /* put ourselves in the population vector */
    peers[welcome.addr().id()] = boost::make_shared<cluster_peer_t>(port, welcome.addr().id());
    us = welcome.addr().id();
    print_peers();

    /* now we need to connect to the peers we received in this welcome packet */
    /* the initial packet we'll send to the new peers */
    *initial.mutable_addr() = welcome.addr();

    for (int i = 0; i < welcome.peers().size(); i++) {
        population::addrinfo addr = welcome.peers(i).addr();
        guarantee(peers.find(addr.id()) == peers.end(), "Duplicate entry found");

        peers[addr.id()] = boost::make_shared<cluster_peer_t>(ip_address_t(addr.ip()), addr.port(), addr.id());

        if (welcome.peers(i).state() == population::LIVE) {
            peers[addr.id()]->connect();
            peers[addr.id()]->write(&initial);
            guarantee(peers[addr.id()]->read(&initial), "Failed to connect to a cluster peer exitting\n");
            peers[addr.id()]->state = cluster_peer_t::connected;
            start_main_srvcs(peers[addr.id()]);
        } else {
            peers[addr.id()]->state = cluster_peer_t::killed;
        }
        print_peers();
    }

    mailbox::intro_msg introduction_header;
    read_protob(&contact_conn, &introduction_header);
    cluster_peer_inpipe_t intro_msg_pipe(&contact_conn, introduction_header.length());
    cond_t to_signal_when_done;
    delegate.reset(startup_function(&intro_msg_pipe, boost::bind(&cond_t::pulse, &to_signal_when_done)));
    to_signal_when_done.wait();
}

void cluster_t::on_tcp_listener_accept(boost::scoped_ptr<tcp_conn_t> &conn) {
    on_thread_t syncer(home_thread());
    /* the protocol buffers we're going to need for this process */
    population::Join_initial    initial;
    if (!read_protob(conn.get(), &initial))
        logINF("Troll peer connected and didn't send a valid first packet\n");

    if (initial.addr().id() == -1) handle_unknown_peer(conn, &initial);
    else handle_known_peer(conn, &initial);
}

void cluster_t::handle_unknown_peer(boost::scoped_ptr<tcp_conn_t> &conn, population::Join_initial *initial) {
    on_thread_t syncer(home_thread());
    logINF("Handle unknown peer\n");
    population::addrinfo            addr;
    population::Join_propose        propose;
    population::Join_mk_official    mk_official;
    population::Join_welcome        welcome;

    addr = initial->addr();
    addr.set_id(peers.size());


    /* propose to the rest of the cluster that we add this new packet to the peers list */
    *propose.mutable_addr() = addr;

    /* first propose the peer to ourselves (or whatever you want to call it) */
    peers[addr.id()] = boost::make_shared<cluster_peer_t>(addr.ip(), addr.port(), us, addr.id());
    peers[addr.id()]->state = cluster_peer_t::join_proposed;
    print_peers();

    for (;;) {
        join_respond_srvc_t *respond_srvc = new join_respond_srvc_t(&addr);
        cluster_peer_t::msg_srvc_ptr respond_srvc_ptr(static_cast<_msg_srvc_t *>(respond_srvc));

        for (int i = 0; i < addr.id(); i++) {
            if (i == us) continue; //don't need to check with ourselves
            if (peers[i]->state > cluster_peer_t::connected) continue;
            wait_on_peer_join(i);
            peers[i]->add_srvc(respond_srvc_ptr);
            peers[i]->write(&propose);
        }

        logINF("Wait for response\n");
        if (respond_srvc->wait()) {
            logINF("Got responses\n");
            break;
        } else {
            /* change the id and try again */
            /* notice we're not deleting the other entry in the peers map.
             * that's going to get deleted when we get proposal that caused our
             * rejection on the other machine (it may well have already
             * happened) */
            addr.set_id(addr.id() + 1);
            *propose.mutable_addr() = addr;
        }
    }

    /* Everyone has agreed to allow the new node to join, time to make it official */
    peers[addr.id()]->state = cluster_peer_t::join_official;
    print_peers();

    join_ack_official_srvc_t *ack_official_srvc = new join_ack_official_srvc_t(&addr);
    cluster_peer_t::msg_srvc_ptr ack_official_srvc_ptr(static_cast<_msg_srvc_t *>(ack_official_srvc));

    *mk_official.mutable_addr() = addr;

    for (std::map<int, boost::shared_ptr<cluster_peer_t> >::iterator it = peers.begin(); it != peers.end(); it++) {
        if (it->first < addr.id() && (it->second->state == cluster_peer_t::join_confirmed || it->second->state == cluster_peer_t::connected)) {
            it->second->add_srvc(ack_official_srvc_ptr);
            it->second->write(&mk_official);
        }
    }

    ack_official_srvc->wait();

    /* Welcome the new node to the cluster */
    *welcome.mutable_addr() = addr;

    for (int i = 0; i < addr.id(); i++) {
        if (peers.find(i) == peers.end()) {
            wait_on_peer_join(i);
        }
        population::peer peer;
        peer.mutable_addr()->set_ip(peers[i]->address.ip_as_uint32());
        peer.mutable_addr()->set_port(peers[i]->port);
        peer.mutable_addr()->set_id(i);
        if (peers[i]->state == cluster_peer_t::killed) { //TODO, this is race conditiony
            peer.set_state(population::KILLED);
        } else {
            peer.set_state(population::LIVE);
        }

        *(welcome.add_peers()) = peer;
    }

    write_protob(conn.get(), &welcome);

    /* Determine how long the introduction will be */
    counting_outpipe_t intro_size_counter;
    delegate->introduce_new_node(&intro_size_counter);

    /* Write the introduction header */
    mailbox::intro_msg intro_msg;
    intro_msg.set_length(intro_size_counter.bytes);
    write_protob(conn.get(), &intro_msg);

    /* Write the introduction body */
    cluster_peer_outpipe_t out_pipe(conn.get(), intro_size_counter.bytes);
    delegate->introduce_new_node(&out_pipe);
}

void cluster_t::handle_known_peer(boost::scoped_ptr<tcp_conn_t> &conn, population::Join_initial *initial) {
    on_thread_t syncer(home_thread());
    if (peers.find(initial->addr().id()) != peers.end() && peers[initial->addr().id()]->state != cluster_peer_t::join_official) {
        logINF("Peer that hasn't been made official attempted to connect\n");
        return;
    }
    peers[initial->addr().id()]->state = cluster_peer_t::connected;
    peers[initial->addr().id()]->conn.swap(conn);
    peers[initial->addr().id()]->write(initial);
    get_cluster()->pulse_peer_join(initial->addr().id());

    print_peers();

    start_main_srvcs(peers[initial->addr().id()]);
}
void cluster_t::start_main_srvcs(boost::shared_ptr<cluster_peer_t> peer) {
    on_thread_t syncer(home_thread());
    coro_t::spawn(boost::bind(&cluster_t::_start_main_srvcs, this, peer));
}

void cluster_t::_start_main_srvcs(boost::shared_ptr<cluster_peer_t> peer) {
    on_thread_t syncer(home_thread());
    cluster_peer_t::msg_srvc_ptr join_propose_srvc = boost::make_shared<join_propose_srvc_t>();
    peer->add_srvc(join_propose_srvc);

    cluster_peer_t::msg_srvc_ptr join_mk_official_srvc = boost::make_shared<join_mk_official_srvc_t>();
    peer->add_srvc(join_mk_official_srvc);

    cluster_peer_t::msg_srvc_ptr kill_propose_srvc = boost::make_shared<kill_propose_srvc_t>();
    peer->add_srvc(kill_propose_srvc);

    cluster_peer_t::msg_srvc_ptr kill_mk_official_srvc = boost::make_shared<kill_mk_official_srvc_t>();
    peer->add_srvc(kill_mk_official_srvc);

    cluster_peer_t::msg_srvc_ptr mailbox_srvc = boost::make_shared<mailbox_srvc_t>();
    peer->add_srvc(mailbox_srvc);

    for (std::vector<cluster_peer_t::msg_srvc_ptr>::iterator it = added_srvcs.begin(); it != added_srvcs.end(); it++)
        peer->add_srvc(*it);

    try {
        peer->start_servicing();
    } 
    catch (linux_tcp_conn_t::read_closed_exc_t) {}
    catch (linux_tcp_conn_t::write_closed_exc_t) {}
    kill_peer(peer->id);
}

void cluster_t::kill_peer(int id) {
    on_thread_t syncer(home_thread());
    logINF("Start kill peer\n");
    population::addrinfo addr;
    population::Kill_propose propose;
    population::Kill_mk_official mk_official;

    guarantee(peers.find(id) != peers.end());
    if (peers[id]->state != cluster_peer_t::connected) return; //someone has beat us to it
    peers[id]->state = cluster_peer_t::kill_proposed;

    peers[id]->fill_in_addr(&addr);
    propose.mutable_addr()->CopyFrom(addr);

    kill_respond_srvc_t *respond_srvc = new kill_respond_srvc_t(&addr);
    cluster_peer_t::msg_srvc_ptr respond_srvc_ptr(static_cast<_msg_srvc_t *>(respond_srvc));

    logINF("Send out proposal\n");
    for (std::map<int, boost::shared_ptr<cluster_peer_t> >::iterator it = peers.begin(); it != peers.end(); it++) {
        if (it->second->state == cluster_peer_t::connected) {
            it->second->add_srvc(respond_srvc_ptr);
            it->second->write(&propose);
        }
    }

    if (respond_srvc->wait()) {
        logINF("Got responses. make it official\n");
        peers[id]->set_state(cluster_peer_t::killed);
        mk_official.mutable_addr()->CopyFrom(addr);
        for (std::map<int, boost::shared_ptr<cluster_peer_t> >::iterator it = peers.begin(); it != peers.end(); it++) {
            if (it->second->state == cluster_peer_t::connected) {
                it->second->write(&mk_official);
            }
        }
    } else {
        not_implemented("We expected everyone to agree to kill a peer at this point");
    }
    print_peers();
}

cluster_t::~cluster_t() {
    rassert(the_cluster == this);
    the_cluster = NULL;

    delete listener; //TODO this is causing a segfault figure out why :(
    not_implemented();
}

void cluster_t::send_message(int peer, int mailbox, cluster_message_t *msg) {
    on_thread_t syncer(home_thread());
    rassert(peers.find(peer) != peers.end());

    if (peers[peer]->state == cluster_peer_t::us) {
        rassert(peer == us);
        /* TODO: What if the mailbox no longer exists? What if it gets destroyed just as we
        send the message? */
        rassert(mailbox_map.map.find(mailbox) != mailbox_map.map.end());
        coro_t::spawn_now(boost::bind(&cluster_mailbox_t::run, mailbox_map.map[mailbox], msg));

    } else {
        on_thread_t syncer(home_thread());
        mailbox::mailbox_msg mbox_msg;

        cluster_peer_t *p = peers[peer].get();
        mutex_acquisition_t locker(&p->write_lock);

        /* Determine how long the message will be */
        counting_outpipe_t msg_size_counter;
        msg->serialize(&msg_size_counter);

        /* Write a message header */
        mbox_msg.set_id(mailbox);
        mbox_msg.set_length(msg_size_counter.bytes);   // Inform the receiver how long the message is supposed to be
#ifndef NDEBUG
        std::string realname = demangle_cpp_name(typeid(*msg).name());
        mbox_msg.set_type(realname);
#endif
        p->write(&mbox_msg);

        /* Write the message body */
        cluster_peer_outpipe_t pipe(p->conn.get(), msg_size_counter.bytes);
        msg->serialize(&pipe);
    }
}

void cluster_t::wait_on_peer_join(int peer_id) {
    if (peers[peer_id]->state == cluster_peer_t::connected || peers[peer_id]->state == cluster_peer_t::us)
        return;
    if (peer_waiters.find(peer_id) == peer_waiters.end()) {
        peer_waiters[peer_id] = boost::make_shared<multi_cond_t>();
    }
    peer_waiters[peer_id]->wait();
}

void cluster_t::pulse_peer_join(int peer_id) {
    if (peer_waiters.find(peer_id) != peer_waiters.end()) {
        peer_waiters[peer_id]->pulse();
        peer_waiters.erase(peer_id);
    }
}

void cluster_t::add_mailbox(cluster_mailbox_t *mbox) {
    on_thread_t syncer(home_thread());
    mailbox_map.map[mailbox_map.head] = mbox;
    mbox->id = mailbox_map.head;
    mailbox_map.head++; //TODO make this recycle ids
}

cluster_mailbox_t *cluster_t::get_mailbox(int i) {
    on_thread_t syncer(home_thread());
    if (mailbox_map.map.find(i) == mailbox_map.map.end())
        return NULL;
    return mailbox_map.map[i];
}

void cluster_t::remove_mailbox(cluster_mailbox_t *mbox) {
    on_thread_t syncer(home_thread());
    mailbox_map.map.erase(mbox->id);
    mbox->id = -1;
}

void cluster_t::add_srvc(cluster_peer_t::msg_srvc_ptr srvc) {
    for (std::map<int, boost::shared_ptr<cluster_peer_t> >::iterator it = peers.begin(); it != peers.end(); it++) {
        it->second->add_srvc(srvc);
    }
    added_srvcs.push_back(srvc);
}

void cluster_t::send_msg(Message *msg, int peer) {
    guarantee(peers.find(peer) != peers.end(), "Sending to unknown peer");
    peers[peer]->write(msg);
}

cluster_address_t::cluster_address_t() :
    peer(-1), mailbox(-1) { }

cluster_address_t::cluster_address_t(const cluster_address_t &addr) :
    peer(addr.peer), mailbox(addr.mailbox) { }

cluster_address_t::cluster_address_t(cluster_mailbox_t *mailbox) :
    peer(get_cluster()->us), mailbox(mailbox->id) { }

void cluster_address_t::send(cluster_message_t *msg) const {
    get_cluster()->send_message(peer, mailbox, msg);
}