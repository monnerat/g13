//
// Created by khampf on 13-05-2020.
//

#include "logo.hpp"
#include <condition_variable>
#include <mutex>
#include <signal.h>
#include <utility>
#include <memory>
#include <log4cpp/OstreamAppender.hh>
#include <libevdev-1.0/libevdev/libevdev.h>
#include "helper.hpp"
#include "g13_keys.hpp"
#include "g13_device.hpp"
#include "g13.hpp"
#include "g13_manager.hpp"
#include "g13_action.hpp"

namespace G13 {
    static std::vector<G13::G13_Device *> g13s;
    bool G13_Manager::running = true;
    libusb_hotplug_callback_handle hotplug_cb_handle[2];
    std::condition_variable wakeup;

    std::map<G13_KEY_INDEX, std::string> g13_key_to_name;
    std::map<std::string, G13_KEY_INDEX> g13_name_to_key;
    std::map<LINUX_KEY_VALUE, std::string> input_key_to_name;
    std::map<std::string, LINUX_KEY_VALUE> input_name_to_key;

    void G13_Manager::start_logging() {
        log4cpp::Appender *appender1 = new log4cpp::OstreamAppender("console", &std::cout);
        appender1->setLayout(new log4cpp::BasicLayout());
        log4cpp::Category &root = log4cpp::Category::getRoot();
        root.addAppender(appender1);

        // TODO: this is for later when --log_file is implemented
        //    log4cpp::Appender *appender2 = new log4cpp::FileAppender("default", "g13d-output.log");
        //    appender2->setLayout(new log4cpp::BasicLayout());
        //    log4cpp::Category &sub1 = log4cpp::Category::getInstance(std::string("sub1"));
        //    sub1.addAppender(appender2);
    }

    // *************************************************************************
    int G13_Manager::OpenAndAddG13(libusb_device *dev) {
        libusb_device_handle *handle;
        int ret = libusb_open(dev, &handle);
        if (ret != 0) {
            G13_ERR("Error opening G13 device");
            return 1;
        }
        if (libusb_kernel_driver_active(handle, 0) == 1) {
            if (libusb_detach_kernel_driver(handle, 0) == 0) {
                G13_ERR("Kernel driver detached");
            }
        }

        ret = libusb_claim_interface(handle, 0);
        if (ret < 0) {
            G13_ERR("Cannot Claim Interface");
            libusb_release_interface(handle, 0);
            libusb_close(handle);
            return 1;
        }
        g13s.push_back(new G13_Device(handle, g13s.size()));
        return 0;
    }

    [[maybe_unused]] void G13_Manager::set_log_level(log4cpp::Priority::PriorityLevel lvl) {
        G13_OUT("set log level to " << lvl);
    }

    void G13_Manager::set_log_level(const std::string &level) {
        log4cpp::Category &root = log4cpp::Category::getRoot();
        try {
            auto numLevel = log4cpp::Priority::getPriorityValue(level);
            root.setPriority(numLevel);
        } catch (std::invalid_argument &e) {
            G13_ERR("unknown log level " << level);
        }
    }

    void G13_Manager::DiscoverG13s(libusb_device **devs,
                                   ssize_t count) {
        for (int i = 0; i < count; i++) {
            libusb_device_descriptor desc;
            int ret = libusb_get_device_descriptor(devs[i], &desc);
            if (ret < 0) {
                G13_ERR("Failed to get device descriptor");
                return;
            }
            if (desc.idVendor == G13::G13_VENDOR_ID && desc.idProduct == G13::G13_PRODUCT_ID) {
                OpenAndAddG13(devs[i]);
            }
        }
    }

    void G13_Manager::Cleanup() {
        G13_OUT("Cleaning up");
        for (auto handle : hotplug_cb_handle) {
            libusb_hotplug_deregister_callback(ctx, handle);
        }
        for (auto &g13 : g13s) {
            g13->cleanup();
            delete g13;
        }
        libusb_exit(ctx);
    }

    G13_Manager::G13_Manager() : ctx(nullptr), devs(nullptr) {
        init_keynames();
    }

    void G13_Manager::init_keynames() {

        int key_index = 0;

        // setup maps to let us convert between strings and G13 key names
        for (auto &name : G13::G13_KEY_STRINGS) {
            G13::g13_key_to_name[key_index] = name;
            G13::g13_name_to_key[name] = key_index;
            G13_DBG("mapping G13 " << name << " = " << key_index);
            key_index++;
        }

        // setup maps to let us convert between strings and linux key names
        for (auto &symbol : G13::G13_SYMBOLS) {
            auto keyname = std::string("KEY_" + std::string(symbol));

            int code = libevdev_event_code_from_name(EV_KEY, keyname.c_str());
            if (code < 0) {
                G13_ERR("No input event code found for " << keyname);
            } else {
                // TODO: this seems to map ok but the result is off
                // assert(keyname.compare(libevdev_event_code_get_name(EV_KEY,code)) == 0);
                // linux/input-event-codes.h

                G13::input_key_to_name[code] = symbol;
                G13::input_name_to_key[symbol] = code;
                G13_DBG("mapping " << symbol << " " << keyname << "=" << code);
            }
        }

        // setup maps to let us convert between strings and linux button names
        for (auto &symbol : G13::G13_BTN_SEQ) {
            auto name = std::string("M" + std::string(symbol));
            auto keyname = std::string("BTN_" + std::string(symbol));
            int code = libevdev_event_code_from_name(EV_KEY, keyname.c_str());
            if (code < 0) {
                G13_ERR("No input event code found for " << keyname);
            } else {
                G13::input_key_to_name[code] = name;
                G13::input_name_to_key[name] = code;
                G13_DBG("mapping " << name << " " << keyname << "=" << code);
            }
        }
    }

    void G13_Manager::signal_handler(int signal) {
        G13_OUT("Caught signal " << signal << " (" << strsignal(signal) << ")");
        running = false;
        wakeup.notify_all();
    }

    std::string G13_Manager::string_config_value(const std::string &name) const {
        try {
            return Helper::find_or_throw(_string_config_values, name);
        } catch (...) {
            return "";
        }
    }

    void G13_Manager::set_string_config_value(const std::string &name, const std::string &value) {
        G13_DBG("set_string_config_value " << name << " = " << Helper::repr(value));
        _string_config_values[name] = value;
    }

    std::string G13_Manager::make_pipe_name(G13::G13_Device *d, bool is_input) const {
        if (is_input) {
            std::string config_base = string_config_value("pipe_in");
            if (!config_base.empty()) {
                if (d->id_within_manager() == 0) {
                    return config_base;
                } else {
                    return config_base + "-" + std::to_string(d->id_within_manager());
                }
            }
            return CONTROL_DIR + "g13-" + std::to_string(d->id_within_manager());
        } else {
            std::string config_base = string_config_value("pipe_out");
            if (!config_base.empty()) {
                if (d->id_within_manager() == 0) {
                    return config_base;
                } else {
                    return config_base + "-" + std::to_string(d->id_within_manager());
                }
            }

            return CONTROL_DIR + "g13-" + std::to_string(d->id_within_manager()) + "_out";
        }
    }

    int LIBUSB_CALL
    G13_Manager::hotplug_callback(struct libusb_context *ctx, struct libusb_device *dev,
                                  libusb_hotplug_event event, void *user_data) {
        static libusb_device_handle *handle = nullptr;
        struct libusb_device_descriptor desc;
        int ret;
        (void) libusb_get_device_descriptor(dev, &desc);
        if (LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED == event) {
            ret = libusb_open(dev, &handle);
            if (LIBUSB_SUCCESS != ret) {
                G13_ERR("Could not open USB device");
            }
            OpenAndAddG13(dev);
        } else if (LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT == event) {
            if (hotplug_cb_handle) {
                libusb_close(handle);
                handle = nullptr;
            }
        } else {
            G13_ERR("Unhandled event " << event);
        }
        return 0;
    }

    G13::LINUX_KEY_VALUE
    G13_Manager::find_g13_key_value(const std::string &keyname) {
        auto i = G13::g13_name_to_key.find(keyname);
        if (i == G13::g13_name_to_key.end()) {
            return G13::BAD_KEY_VALUE;
        }
        return i->second;
    }

    G13::LINUX_KEY_VALUE
    G13_Manager::find_input_key_value(const std::string &keyname) const {
        // if there is a KEY_ prefix, strip it off
        if (!strncmp(keyname.c_str(), "KEY_", 4)) {
            return find_input_key_value(keyname.c_str() + 4);
        }

        auto i = G13::input_name_to_key.find(keyname);
        if (i == G13::input_name_to_key.end()) {
            return G13::BAD_KEY_VALUE;
        }
        return i->second;
    }

    std::string G13_Manager::find_input_key_name(G13::LINUX_KEY_VALUE v) {
        try {
            return Helper::find_or_throw(G13::input_key_to_name, v);
        } catch (...) {
            return "(unknown linux key)";
        }
    }

    void G13_Manager::setupDevice(G13::G13_Device *g13) {
        g13->register_context(ctx);

        if (!logo_filename.empty()) {
            g13->write_lcd_file(logo_filename);
        }

        G13_OUT("Active Stick zones ");
        g13->stick().dump(std::cout);

        std::string config_fn = string_config_value("config");
        if (!config_fn.empty()) {
            G13_OUT("config_fn = " << config_fn);
            g13->read_config_file(config_fn);
        }
    }

    std::string G13_Manager::find_g13_key_name(G13::G13_KEY_INDEX v) {
        try {
            return Helper::find_or_throw(G13::g13_key_to_name, v);
        } catch (...) {
            return "(unknown G13 key)";
        }
    }

    void G13_Manager::display_keys() {
        typedef std::map<std::string, int> mapType;
        G13_OUT("Known keys on G13:");
        G13_OUT(Helper::map_keys_out(G13::g13_name_to_key));

        G13_OUT("Known keys to map to:");
        G13_OUT(Helper::map_keys_out(G13::input_name_to_key));
    }

    int G13_Manager::run() {
        static const int class_id = LIBUSB_HOTPLUG_MATCH_ANY;

        display_keys();

        ssize_t cnt;
        int ret;

        ret = libusb_init(&ctx);
        if (ret != LIBUSB_SUCCESS) {
            G13_ERR("libusb initialization error: " << ret);
            Cleanup();
            return EXIT_FAILURE;
        }
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, 3);

        if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
            cnt = libusb_get_device_list(ctx, &devs);
            if (cnt < 0) {
                G13_ERR("Error while getting device list");
                Cleanup();
                return EXIT_FAILURE;
            }

            DiscoverG13s(devs, cnt);
            libusb_free_device_list(devs, 1);
            G13_OUT("Found " << g13s.size() << " G13s");
            if (g13s.empty()) {
                G13_ERR("Unable to open any device");
                Cleanup();
                return EXIT_FAILURE;
            }

            for (auto &g13 : g13s) {
                setupDevice(g13);
            }
        } else {
            G13_DBG("Registering USB hotplug callbacks");

            ret = libusb_hotplug_register_callback(ctx, (LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED),
                                                   LIBUSB_HOTPLUG_ENUMERATE,
                                                   G13::G13_VENDOR_ID, G13::G13_PRODUCT_ID,
                                                   class_id,
                                                   hotplug_callback,
                                                   nullptr, &hotplug_cb_handle[0]);
            if (ret != LIBUSB_SUCCESS) {
                G13_ERR("Error registering hotplug callback 1");
            }
            ret = libusb_hotplug_register_callback(ctx, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                                   LIBUSB_HOTPLUG_NO_FLAGS,
                                                   G13::G13_VENDOR_ID, G13::G13_PRODUCT_ID,
                                                   class_id,
                                                   hotplug_callback,
                                                   nullptr, &hotplug_cb_handle[1]);
            if (ret != LIBUSB_SUCCESS) {
                G13_ERR("Error registering hotplug callback 2");
            }
        }

        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        do {
            if (g13s.empty()) {
                std::mutex wakemutex;
                std::unique_lock<std::mutex> wakelock{wakemutex};
                G13_OUT("Waiting...");
                wakeup.wait(wakelock);
            }
            for (auto &g13 : g13s) {
                int status = g13->read_keys();
                g13->read_commands();
                if (status < 0) {
                    running = false;
                }
            }
        } while (running);

        Cleanup();
        G13_OUT("Exit");
        return EXIT_SUCCESS;
    }
}