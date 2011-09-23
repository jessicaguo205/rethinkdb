#ifndef __CLUSTERING_BACKFILLER_HPP__
#define __CLUSTERING_BACKFILLER_HPP__

#include <map>

#include "clustering/listener.hpp"

/* If you construct a `backfiller_t` for a given listener, then the listener
will announce its existence in the metadata so that newly created listeners can
backfill from it. */
template<class protocol_t>
struct backfiller_t :
    public home_thread_mixin_t
{
    backfiller_t(
            mailbox_cluster_t *c,
            listener_t *l
            boost::shared_ptr<metadata_read_view_t<branch_history_t<protocol_t> > > bh,
            boost::shared_ptr<ready_store_t<protocol_t> > s,
            branch_id_t sb,
            boost::shared_ptr<metadata_readwrite_view_t<resource_metadata_t<backfiller_metadata_t<protocol_t> > > > md_view) :
        cluster(c), branch_history(bh),
        store(s), store_branch(sb),
        backfill_mailbox(cluster, boost::bind(&backfiller_t::on_backfill, this, _1, _2, _3, _4, auto_drainer_t::lock_t(&drainer))),
        cancel_backfill_mailbox(cluster, boost::bind(&backfiller_t::on_cancel_backfill, this, _1, auto_drainer_t::lock_t(&drainer))),
        advertisement(cluster, md_view, backfiller_metadata_t<protocol_t>(backfill_mailbox.get_address(), cancel_backfill_mailbox.get_address()))
    {
        /* The store's region must match the branch it's supposedly on, and its
        current timestamp shouldn't be before when the branch was created. */
        rassert(store->get_region() == branch_find(branch_history, store_branch).region);
        rassert(store->get_timestamp() >= branch_find(branch_history, store_branch).initial_timestamp);
    }

private:
    typedef typename backfiller_metadata_t<protocol_t>::backfill_session_id_t session_id_t;

    void on_backfill(
        session_id_t session_id,
        typename protocol_t::store_t::backfill_request_t request,
        typename async_mailbox_t<void(typename protocol_t::store_t::backfill_chunk_t)>::address_t chunk_cont,
        typename async_mailbox_t<void(state_timestamp_t)>::address_t end_cont,
        auto_drainer_t::lock_t keepalive) {

        assert_thread();

        /* Set up a local interruptor cond and put it in the map so that this
        session can be interrupted if the backfillee decides to abort */
        cond_t local_interruptor;
        map_insertion_sentry_t<session_id_t, cond_t *> be_interruptible(&local_interruptors, session_id, &local_interruptor);

        /* Set up a cond that gets pulsed if we're interrupted either way */
        wait_any_t interrupted(&local_interruptor, keepalive.get_drain_signal());

        /* Calling `send_chunk()` will send a chunk to the backfillee. We need
        to cast `send()` to the correct type before calling `boost::bind()` so
        that C++ will find the correct overload. */
        void (*send_cast_to_correct_type)(
            mailbox_cluster_t *,
            typename async_mailbox_t<void(typename protocol_t::store_t::backfill_chunk_t)>::address_t,
            const typename protocol_t::store_t::backfill_chunk_t &) = &send;
        boost::function<void(typename protocol_t::store_t::backfill_chunk_t)> send_fun =
            boost::bind(send_cast_to_correct_type, cluster, chunk_cont, _1);

        /* Perform the backfill */
        bool success;
        state_timestamp_t end;
        try {
            end = store->backfiller(
                request,
                send_fun,
                &interrupted);
            success = true;
        } catch (interrupted_exc_t) {
            rassert(interrupted.is_pulsed());
            success = false;
        }

        /* Send the confirmation */
        if (success) {
            send(cluster, end_cont, end);
        }
    }

    void on_cancel_backfill(session_id_t session_id, UNUSED auto_drainer_t::lock_t) {

        assert_thread();

        typename std::map<session_id_t, cond_t *>::iterator it =
            local_interruptors.find(session_id);
        if (it != local_interruptors.end()) {
            (*it).second->pulse();
        }
    }

    mailbox_cluster_t *cluster;
    boost::shared_ptr<metadata_read_view_t<branch_history_t<protocol_t> > > branch_history;

    boost::shared_ptr<ready_store_t<protocol_t> > store;
    branch_id_t store_branch;

    auto_drainer_t drainer;
    std::map<session_id_t, cond_t *> local_interruptors;

    typename backfiller_metadata_t<protocol_t>::backfill_mailbox_t backfill_mailbox;
    typename backfiller_metadata_t<protocol_t>::cancel_backfill_mailbox_t cancel_backfill_mailbox;

    resource_advertisement_t<backfiller_metadata_t<protocol_t> > advertisement;
};

#endif /* __CLUSTERING_BACKFILLER_HPP__ */
