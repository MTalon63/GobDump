#pragma once

/**
 * @file event_bus.h
 * @brief Event bus implementation
 */

#include <cstdint>
#include <functional>
#include <string>
#include <typeinfo>
#include <vector>

namespace satdump
{
    /**
     * @brief Very simple event bus implementation using
     * std::function and typeid.
     * All this does is fire any registered handler
     * when called.
     *
     * std::any could not be used as it can mess up
     * over several .so.
     * (typeinfo does NOT allow casting between
     * several interpretations of the exact same
     * struct)
     */
    class EventBus
    {
    private:
        struct HandlerEntry
        {
            uint64_t id;
            std::string type_name;
            std::function<void(void *)> fun;
        };

        std::vector<HandlerEntry> all_handlers;
        uint64_t next_id = 1;

    public:
        /**
         * @brief Register a handler function to be called
         * when a specific event is fired.
         *
         * @param handler_fun function to register
         * @return ID to pass to unregister_handler()
         */
        template <typename T>
        uint64_t register_handler(std::function<void(T)> handler_fun)
        {
            uint64_t id = next_id++;
            all_handlers.push_back({id, std::string(typeid(T).name()), [handler_fun](void *raw)
                                    {
                                        T evt = *((T *)raw); // Cast struct to original type
                                        handler_fun(evt);    // Call real handler
                                    }});
            return id;
        }

        /**
         * @brief Unregister a previously registered handler.
         * Must not be called while the bus is being fired
         * from another thread.
         *
         * @param id ID returned by register_handler()
         */
        void unregister_handler(uint64_t id)
        {
            for (auto it = all_handlers.begin(); it != all_handlers.end(); ++it)
                if (it->id == id)
                {
                    all_handlers.erase(it);
                    return;
                }
        }

        /**
         * @brief Trigger an event, called every registered
         * handler
         *
         * @param evt event struct
         */
        template <typename T>
        void fire_event(T evt)
        {
            for (HandlerEntry &h : all_handlers)                          // Iterate through all registered functions
                if (std::string(typeid(T).name()) == h.type_name)         // Check struct type is the same
                    h.fun((void *)&evt);                                  // Fire handler up
        }

        /**
         * @brief Trigger an event, called every registered
         * handler. Allows specifying the event name.
         * Used by task scheduler.
         *
         * @param evt event struct
         * @param evt_name event name to trigger
         */
        void fire_event(void *evt, std::string evt_name)
        {
            for (HandlerEntry &h : all_handlers) // Iterate through all registered functions
                if (evt_name == h.type_name)     // Check struct type is the same
                    h.fun(evt);                  // Fire handler up
        }
    };
} // namespace satdump