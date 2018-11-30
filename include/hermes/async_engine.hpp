#ifndef __HERMES_ASYNC_ENGINE_HPP__
#define __HERMES_ASYNC_ENGINE_HPP__

// C++ includes
#include <string>
#include <unordered_map>
#include <atomic>
#include <cassert>
#include <thread>
#include <mutex>
#include <set>
#include <vector>
#include <utility>
#include <future>
#include <chrono>
#include <algorithm>

// C includes
#include <mercury.h>
#include <mercury_macros.h>
#include <mercury_hash_string.h>

// project includes
#if __cplusplus == 201103L
#include <hermes/make_unique.hpp>
#endif // __cplusplus == 201103L

#include <hermes/endpoint.hpp>
#include <hermes/exposed_memory.hpp>
#include <hermes/logging.hpp>
#include <hermes/transport.hpp>

#include <hermes/detail/address.hpp>
#include <hermes/detail/mercury_utils.hpp>
#include <hermes/detail/request_registrar.hpp>
#include <hermes/detail/request_status.hpp>

namespace hermes {

// defined elsewhere
template <typename Request> class rpc_handle;
template <typename Request> class request;

using endpoint_set = std::vector<endpoint>;

template <typename Request>
using output_set = std::vector<typename Request::output_type>;

/** public */
class async_engine {

    struct lookup_ctx {

        explicit lookup_ctx(const hg_class_t* const hg_class) : 
            m_lookup_finished(false),
            m_hg_class(hg_class) { }

        ~lookup_ctx() {
        }

        bool m_lookup_finished;
        const hg_class_t* const m_hg_class;
        hg_addr_t m_hg_addr;
        hg_return_t m_hg_ret;
    };

public:
    async_engine(const async_engine& other) = delete;
    async_engine(async_engine&& rhs) = default;
    async_engine& operator=(const async_engine& other) = delete;
    async_engine& operator=(async_engine&& rhs) = default;

    /**
     * Initialize the Hermes asynchronous engine.
     **/
    async_engine(transport transport_type,
                 const std::string& bind_address = "",
                 bool listen = false) :
        m_shutdown(false),
        m_listen(listen),
        m_transport(transport_type) {

        m_hg_class = 
                detail::initialize_mercury(
                        get_transport_prefix(m_transport), 
                        bind_address, 
                        m_listen);

        DEBUG2("m_hg_class: {}", static_cast<void*>(m_hg_class));

        m_hg_context = detail::create_mercury_context(m_hg_class);

        DEBUG2("m_hg_context: {}", static_cast<void*>(m_hg_context));

        if(m_listen) {
            m_self_address = 
                std::make_unique<detail::address>(
                    detail::address::self_address(m_hg_class));

            DEBUG("Self address: {}", m_self_address->to_string());
        }

        DEBUG("Registering RPCs");
        register_rpcs();
    }

    /**
     * Destroy the Mercury asynchronous engine.
     */
    ~async_engine() {

        // we need to release the hg_addr_t contained in m_self_address
        // so that HG_Context_destroy() and HG_Finalize() work as expected
        m_self_address.reset();

        DEBUG("Destroying Mercury asynchronous engine");
        DEBUG("  Stopping runners");

        m_shutdown = true;

        if(m_runner.joinable()) {
            m_runner.join();
        }

        DEBUG("  Cleaning address cache");
        {
            std::lock_guard<std::mutex> lock(m_addr_cache_mutex);
            m_address_cache.clear();
        }

        if(m_hg_context != NULL) {
            DEBUG("  Destroying context");
            const auto err = HG_Context_destroy(m_hg_context);

            if(err != HG_SUCCESS) {
                // can't throw here
                ERROR("Failed to destroy context: {}\n",
                      HG_Error_to_string(err));
            }
        }

        if(m_hg_class != NULL) {
            DEBUG("  Finalizing transport layer");
            const auto err = HG_Finalize(m_hg_class);

            if(err != HG_SUCCESS) {
                // can't throw here
                ERROR("Failed to shut down transport layer: {}\n",
                      HG_Error_to_string(err));
            }
        }
    }

    hg_return_t
    wait_on(const lookup_ctx& ctx) const {
        hg_return ret;
        unsigned int actual_count;

        while(!ctx.m_lookup_finished) {
            do {
                ret = HG_Trigger(m_hg_context,
                                0,
                                1,
                                &actual_count);
            } while((ret == HG_SUCCESS) &&
                    (actual_count != 0) &&
                    !ctx.m_lookup_finished);

            if(!ctx.m_lookup_finished) {
                ret = HG_Progress(m_hg_context,
                                  100);

                if(ret != HG_SUCCESS && ret != HG_TIMEOUT) {
                    WARNING("Unexpected return code {} from HG_Progress: {}",
                            ret, HG_Error_to_string(ret));
                    return ret;
                }
            }
        }

        return HG_SUCCESS;
    }

    endpoint
    lookup(const std::string& addr) const {

        DEBUG("Looking up endpoint \"{}\"", addr);

        // address does not contain a prefix
        if(addr.find("://") != std::string::npos) {
            throw std::runtime_error("Explicit specification of transport "
                                     "protocol in addresses is not supported.");
        }

        const std::string transport_address(
                get_transport_lookup_prefix(m_transport) + addr);

        {
            std::lock_guard<std::mutex> lock(m_addr_cache_mutex);

            const auto it = m_address_cache.find(transport_address);

            if(it != m_address_cache.end()) {
                DEBUG("Endpoint \"{}\" cached {}", 
                      addr, fmt::ptr(it->second.get()));
                return endpoint(it->second);
            }
        }

        assert(m_hg_class);
        assert(m_hg_context);

        lookup_ctx ctx(m_hg_class);

        hg_return_t ret = HG_Addr_lookup(
            // Mercury execution context
            const_cast<hg_context_t*>(m_hg_context),
            // pointer to callback
            [](const struct hg_cb_info* cbi) -> hg_return_t {

                auto* ctx = static_cast<lookup_ctx*>(cbi->arg);

                ctx->m_lookup_finished = true;
                ctx->m_hg_addr = cbi->info.lookup.addr;
                ctx->m_hg_ret = cbi->ret;

                if(cbi->ret != HG_SUCCESS) {
                    return cbi->ret;
                }

                return HG_SUCCESS;
            },
            // pointer to data passed to callback
            //static_cast<void*>(ctx.get()),
            static_cast<void*>(&ctx),
            // name to lookup
            transport_address.c_str(),
            // pointer to returned operation ID
            HG_OP_ID_IGNORE);

        DEBUG2("HG_Addr_lookup({}, {}, {}, {}, HG_OP_ID_IGNORE) = {}", 
               fmt::ptr(m_hg_context), "foo", fmt::ptr(&ctx), 
               transport_address, HG_Error_to_string(ret));

        if(ret != HG_SUCCESS) {
            throw std::runtime_error("Failed to lookup target");
        }

        ret = wait_on(ctx);

        if(ret != HG_SUCCESS) {
            DEBUG("Lookup request failed");
            throw std::runtime_error("Failed to lookup target");
        }

        assert(ctx.m_lookup_finished);
        DEBUG("Lookup request succeeded [hg_addr: {}]",
              fmt::ptr(ctx.m_hg_addr));

        
        std::pair<decltype(m_address_cache)::iterator, bool> rv;

        {
            std::lock_guard<std::mutex> lock(m_addr_cache_mutex);
            rv = m_address_cache.emplace(
                    addr,
                    std::make_shared<detail::address>(
                        m_hg_class, ctx.m_hg_addr));
        }

        return endpoint(rv.first->second);
    }

    endpoint_set
    lookup(std::initializer_list<std::string>&& addrs) const {


        std::set<std::string> unique_addrs(addrs);

        endpoint_set endps;

        // TODO: this waits on each individual lookup. Make it so that
        // all lookups are posted to mercury and we only wait once on
        // total completion
        for(const auto addr : unique_addrs) {
            endps.emplace_back(lookup(addr));
        }

        return endps;
    }

    template <typename Request, typename Callable>
    void
    register_handler(Callable&& handler) {

        const auto descriptor = 
            std::static_pointer_cast<detail::request_descriptor<Request>>(
                detail::registered_requests().at(Request::public_id));

        if(!descriptor) {
            throw std::runtime_error("Failed to register handler for request "
                                     "of unknown type");
        }

        descriptor->set_user_handler(std::forward<Callable>(handler));
    }

    /**
     * Starts the Mercury asynchronous engine
     */
    void
    run() {

        DEBUG("Starting Mercury asynchronous engine");

        assert(m_hg_class);
        assert(m_hg_context);

        m_runner = std::thread(&async_engine::progress_thread, this);
    }

    template <typename BufferSequence>
    exposed_memory
    expose(BufferSequence&& bufseq, 
           access_mode mode) {

        assert(m_hg_context);
        assert(m_hg_class);

        return {m_hg_class, mode, std::forward<BufferSequence>(bufseq)};
    }


    template <typename Request, typename Endpoint, typename... Args>
    typename Request::handle_type
    post(Endpoint&& target,
         Args&&... args) {

        using Input = typename Request::input_type;
        using Handle = typename Request::handle_type;

        DEBUG2("Posting RPC to endpoint {}", target.address()->to_string());

        auto handle = Handle(m_hg_context, 
                             {target.address()},
                             Input(std::forward<Args>(args)...));

        const auto& ctx = handle.m_ctxs[0];

        hg_return ret = detail::post_to_mercury(ctx.get());

        if(ret != HG_SUCCESS) {

            ctx->m_status = detail::request_status::failed;

            throw std::runtime_error("Failed to post RPC: " + 
                    std::string(HG_Error_to_string(ret)));
        }

        return handle;
    }


    template <typename Request, typename EndpointSet, typename... Args>
    typename Request::handle_type
    broadcast(EndpointSet&& targets,
              Args&&... args) {

        using Input = typename Request::input_type;
        // using Output = typename Request::output_type;
        using Handle = typename Request::handle_type;

        DEBUG2("Posting RPC to multiple endpoints");

        std::vector<std::shared_ptr<detail::address>> addrs;
        std::transform(targets.begin(), targets.end(),
                       std::back_inserter(addrs),
                       [](const endpoint& endp) {
                           return endp.address();
                       });

        auto handle = Handle(m_hg_context, 
                             addrs, 
                             Input(std::forward<Args>(args)...));

        // TODO: move to a private function in rpc_handle class
        std::size_t i = 0;

        for(const auto& ctx : handle.m_ctxs) {

            //    auto input = MercuryInput(ctx->m_input);
            hg_return ret = detail::post_to_mercury(ctx.get());

            // if this submission fails, cancel all previously posted RPCs
            if(ret != HG_SUCCESS) {

                ctx->m_status = detail::request_status::cancelled;

                for(std::size_t j = 0; j < i; ++j) {


                    ret = HG_Cancel(handle.m_ctxs[i]->m_handle);

                    if(ret != HG_SUCCESS) {
                        WARNING("Failed to cancel RPC: {}", 
                                HG_Error_to_string(ret));
                    }

                }

                throw std::runtime_error("Failed to post RPC: " + 
                        std::string(HG_Error_to_string(ret)));
            }
            
            ++i;
        }

        return handle;
        
    }

    template <typename Input, typename Callable>
    void async_pull(const exposed_memory& src,
                    const exposed_memory& dst,
                    request<Input>&& req,
                    Callable&& user_callback) {

        hg_bulk_t local_bulk_handle = dst.mercury_bulk_handle();
        hg_bulk_t remote_bulk_handle = src.mercury_bulk_handle();

        assert(local_bulk_handle != HG_BULK_NULL);
        assert(remote_bulk_handle != HG_BULK_NULL);

        // We need to allow custom user callbacks, but the Mercury API 
        // restricts us in the prototypes that we can use. Since we don't
        // want users to bother with Mercury internals, // we register our own
        // completion_callback() lambda and we dynamically allocate
        // a transfer_context with all the required information that we
        // propagate through Mercury using the arg field in HG_Bulk_transfer().
        // Once our callback is invoked, we can unpack the information, delete
        // the transfer_context and invoke the actual user callback
        struct transfer_context {
            transfer_context(request<Input>&& req, 
                             Callable&& user_callback) :
                m_request(std::move(req)),
                m_user_callback(user_callback) { }

            ~transfer_context() {
                DEBUG("{}()", __func__);
            }

            request<Input> m_request;
            // XXX For some reason, declaring m_user_callback as:
            //    Callable m_user_callback;
            // causes a weird bug with GCC 4.9 where any variables captured 
            // by value in the lambda get corrupted. Replacing it with 
            // std::function fixes this
            std::function<void(request<Input>&&)> m_user_callback;
        };

        auto* ctx =
            new transfer_context(std::move(req), 
                                 user_callback);

        const auto completion_callback =
                [](const struct hg_cb_info* cbi) -> hg_return_t {

                    if(cbi->ret != HG_SUCCESS) {
                        DEBUG("Bulk transfer failed");
                        return cbi->ret;
                    }

                    // make sure that ctx is freed regardless of what might 
                    // happen in m_user_callback()
                    const auto ctx = 
                        std::unique_ptr<transfer_context>(
                                reinterpret_cast<transfer_context*>(cbi->arg));

                    ctx->m_user_callback(std::move(ctx->m_request));

                    return HG_SUCCESS;
                };

        detail::mercury_bulk_transfer(ctx->m_request.m_handle,
                                      remote_bulk_handle, 
                                      local_bulk_handle, 
                                      ctx,
                                      completion_callback);
    }

    template <typename Request, typename... Args>
    void
    respond(request<Request>&& req, 
            Args&&... args) const {

        using output_type = typename Request::output_type;
        using mercury_output_type = typename Request::mercury_output_type;

        detail::mercury_respond(
                std::move(req), 
                mercury_output_type(
                    output_type(std::forward<Args>(args)...)));
    }

private:
    /**
     * Register RPCs into Mercury so that they can be called by the clients 
     * of the asynchronous engine
     */
    void
    register_rpcs() {

        assert(m_hg_class);
        assert(m_hg_context);

        for(auto&& kv : detail::registered_requests()) {

            // auto&& id = kv.first;
            auto&& descriptor = kv.second;

            DEBUG("**** registered: {}, {}, {}, {}, {}", 
                    descriptor->m_id,
                    descriptor->m_mercury_id,
                    descriptor->m_name,
                    reinterpret_cast<void*>(descriptor->m_mercury_input_cb),
                    reinterpret_cast<void*>(descriptor->m_mercury_output_cb));

            detail::register_mercury_rpc(m_hg_class, descriptor, m_listen);
        }
    }

    /**
     * Dedicated thread that drives Mercury progress
     */
    void
    progress_thread() {

        assert(m_hg_class);
        assert(m_hg_context);

        hg_return ret;
        unsigned int actual_count;

        while(!m_shutdown) {
            do {
                ret = HG_Trigger(m_hg_context,
                                 0,
                                 1,
                                 &actual_count);

                DEBUG4("HG_Trigger(context={}, timeout={}, max_count={}, "
                       "actual_count={}) = {}", fmt::ptr(m_hg_context), 0, 1,
                       actual_count, HG_Error_to_string(ret));

            } while((ret == HG_SUCCESS) &&
                    (actual_count != 0) &&
                    !m_shutdown);

            if(!m_shutdown) {
                ret = HG_Progress(m_hg_context, 100);

                DEBUG4("HG_Progress(context={}, timeout={}) = {}", 
                       fmt::ptr(m_hg_context), 100, HG_Error_to_string(ret));

                if(ret != HG_SUCCESS && ret != HG_TIMEOUT) {
                    FATAL("Unexpected return code {} from HG_Progress: {}",
                          ret, HG_Error_to_string(ret));
                }
            }
        }
    }

    std::atomic<bool> m_shutdown;
    hg_class_t* m_hg_class;
    hg_context_t* m_hg_context;
    bool m_listen;
    const transport m_transport;
    std::unique_ptr<detail::address> m_self_address;
    std::thread m_runner;

    mutable std::mutex m_addr_cache_mutex;
    mutable std::unordered_map<
        std::string, 
        std::shared_ptr<detail::address>> m_address_cache;
}; // class async_engine

} // namespace hermes

#endif // __HERMES_ASYNC_ENGINE_HPP__
