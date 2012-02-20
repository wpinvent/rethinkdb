#include "unittest/gtest.hpp"
#include "clustering/registrar.hpp"
#include "clustering/registrant.hpp"
#include "unittest/clustering_utils.hpp"
#include "unittest/dummy_metadata_controller.hpp"
#include "unittest/unittest_utils.hpp"

namespace unittest {

namespace {

class monitoring_controller_t {

public:
    class registrant_t {
    public:
        registrant_t(monitoring_controller_t *c, std::string data) : parent(c) {
            EXPECT_FALSE(parent->has_registrant);
            parent->has_registrant = true;
            parent->registrant_data = data;
        }
        ~registrant_t() {
            EXPECT_TRUE(parent->has_registrant);
            parent->has_registrant = false;
        }
        monitoring_controller_t *parent;
    };

    monitoring_controller_t() : has_registrant(false) { }
    bool has_registrant;
    std::string registrant_data;
};

/* `let_stuff_happen()` delays for some time to let events occur */
void let_stuff_happen() {
    nap(1000);
}

}   /* anonymous namespace */

/* `Register` tests registration, updating, and deregistration of a single
registrant. */

void run_register_test() {

    simple_mailbox_cluster_t cluster;

    monitoring_controller_t controller;
    registrar_t<std::string, monitoring_controller_t *, monitoring_controller_t::registrant_t> registrar(
        cluster.get_mailbox_manager(),
        &controller);

    simple_directory_manager_t<boost::optional<registrar_business_card_t<std::string> > > metadata_controller(
        &cluster,
        boost::optional<registrar_business_card_t<std::string> >(registrar.get_business_card())
        );

    EXPECT_FALSE(controller.has_registrant);

    {
        registrant_t<std::string> registrant(
            cluster.get_mailbox_manager(),
            metadata_controller.get_root_view()->get_peer_view(cluster.get_connectivity_service()->get_me()),
            "hello");
        let_stuff_happen();

        EXPECT_FALSE(registrant.get_failed_signal()->is_pulsed());
        EXPECT_TRUE(controller.has_registrant);
        EXPECT_EQ("hello", controller.registrant_data);
    }
    let_stuff_happen();

    EXPECT_FALSE(controller.has_registrant);
}
TEST(ClusteringRegistration, Register) {
    run_in_thread_pool(&run_register_test);
}

/* `RegistrarDeath` tests the case where the registrar dies while the registrant
is registered. */

void run_registrar_death_test() {

    simple_mailbox_cluster_t cluster;

    monitoring_controller_t controller;

    /* Set up `registrar` in a `boost::scoped_ptr` so we can destroy it whenever
    we want to */
    boost::scoped_ptr<registrar_t<std::string, monitoring_controller_t *, monitoring_controller_t::registrant_t> > registrar(
        new registrar_t<std::string, monitoring_controller_t *, monitoring_controller_t::registrant_t>(
            cluster.get_mailbox_manager(),
            &controller
        ));

    EXPECT_FALSE(controller.has_registrant);

    simple_directory_manager_t<boost::optional<registrar_business_card_t<std::string> > > metadata_controller(
        &cluster,
        boost::optional<registrar_business_card_t<std::string> >(registrar->get_business_card())
        );

    registrant_t<std::string> registrant(
        cluster.get_mailbox_manager(),
        metadata_controller.get_root_view()->get_peer_view(cluster.get_connectivity_service()->get_me()),
        "hello");
    let_stuff_happen();

    EXPECT_FALSE(registrant.get_failed_signal()->is_pulsed());
    EXPECT_TRUE(controller.has_registrant);
    EXPECT_EQ("hello", controller.registrant_data);

    /* Kill the registrar */
    {
        directory_write_service_t::our_value_lock_acq_t lock(&metadata_controller);
        metadata_controller.get_root_view()->set_our_value(
            boost::optional<registrar_business_card_t<std::string> >(),
            &lock);
    }
    registrar.reset();

    let_stuff_happen();

    EXPECT_TRUE(registrant.get_failed_signal()->is_pulsed());
    EXPECT_FALSE(controller.has_registrant);
}
TEST(ClusteringRegistration, RegistrarDeath) {
    run_in_thread_pool(&run_registrar_death_test);
}

}   /* namespace unittest */