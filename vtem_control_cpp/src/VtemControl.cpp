#include "modbus/modbus-tcp.h"
#include "VtemControl.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>


namespace {
    const int address_input_start = 45392;
    const int address_output_start = 40001;
    const int cpx_input_offset = 3;
    const int cpx_output_offset = 2;
    const int num_slots = 8;
}

vtem_control::VtemControl::VtemControl(const char *node, const char *service) {
    // Not connected.
    connected_ = false;

    // Resize buffer space.
    // Input buffer for each valve is of the format (actual, setpoint, diagnostic).
    input_buffer_.resize(2*2 * num_slots);
    output_buffer_.resize(2 * num_slots);

    // Create Modbus context.
    ctx_ = modbus_new_tcp_pi(node, service);
}

vtem_control::VtemControl::~VtemControl() {
    // Close if not yet closed.
    if (connected_) {
        disconnect();
    }

    // Deallocate Modbus context.
    modbus_free(ctx_);
}

bool vtem_control::VtemControl::connect() {
    if (modbus_connect(ctx_) == -1) {
        std::cout << "Connection failed" << std::endl;
        connected_ = false;
        return false;
    }
    connected_ = true;
    return true;
}

bool vtem_control::VtemControl::disconnect() {
    if (connected_) {
        modbus_close(ctx_);
        connected_ = false;
        return true;
    }

    return false;
}

bool vtem_control::VtemControl::get_single_motion_app(int slot_idx, int &motion_app_id, int &valve_state) {
    ensure_connection();

    const auto addr = address_input_start + cpx_input_offset + 2*3*slot_idx;
    uint16_t status; // this should read two bytes containing the slot status information
    
    if (modbus_read_registers(ctx_, addr, 2, &status) == -1) {
        throw std::runtime_error("Failed to read slot status register.");
    }

    /* example to extract individual bytes: 
    // mask: 0xFF (only selects first byte)
    // shift operator: delete the first 8 bits
    uint8_t first_byte = status & 0xFF;
    uint8_t second_byte = (status >> 8) & 0xFF;
    */

    motion_app_id = status & 0x3F;
    valve_state = (status >> 6) & 0x03;
 
    /*  extract bits 6-8 from first byte to read actual valve state
        for motion app 03:
            00: both valves are inactive
            01: second valve is active
            02: first valve is active
            03: both valves are active
    */

    return true;
}

bool vtem_control::VtemControl::set_single_motion_app(int slot_idx, int motion_app_id = 61, int app_control = 0) {
    /*  App control for motion app 03 (proportional pressure regulation):
            00: both valves are inactive
            01: second valve is active
            02: first valve is active
            03: both valves are active
    */

    ensure_connection();

    uint16_t command_first_byte = (app_control << 6) | motion_app_id;
    uint16_t command_second_byte = 0;
    uint16_t command = (command_second_byte << 8) | command_first_byte;
    
    const auto addr = address_output_start + cpx_output_offset + 3*slot_idx;
    if (modbus_write_register(ctx_, addr, command) == -1) {
        throw std::runtime_error("Failed to write register for setting motion app.");
    }

    return true;
}

bool vtem_control::VtemControl::set_all_motion_apps(int motion_app_id = 61, int app_control = 0) {
    for (auto slot_idx = 0; slot_idx < (num_slots); slot_idx++) {
        if (!set_single_motion_app(slot_idx, motion_app_id, app_control)) {
            return false;
        }
    }
    return true;
}

bool vtem_control::VtemControl::activate_pressure_regulation(int slot_idx = -1) {
    if (slot_idx == -1) {
        return set_all_motion_apps(3, 3);
    } else {
        return set_single_motion_app(slot_idx, 3, 3);
    }
}

bool vtem_control::VtemControl::deactivate_pressure_regulation(int slot_idx = -1) {
    if (slot_idx == -1) {
        return set_all_motion_apps(61, 0);
    } else {
        return set_single_motion_app(slot_idx, 61, 0);
    }
}

void vtem_control::VtemControl::ensure_connection() const {
    if (!connected_) {
        throw std::runtime_error("Operation requires a connection.");
    }
}

void vtem_control::VtemControl::ensure_motion_app(int slot_idx, int des_motion_app_id, int des_valve_state) {
    int motion_app_id, valve_state;

    get_single_motion_app(slot_idx, motion_app_id, valve_state);

    if (motion_app_id != des_motion_app_id) {
        throw std::runtime_error("Operation requires activating the suitable motion app");
    }

    if (valve_state != des_valve_state) {
        throw std::runtime_error("Operation requires setting the desired valve state");
    }
}

int vtem_control::VtemControl::get_slot_idx_from_valve_idx(const int valve_idx) const {
    int slot_idx = std::floor(valve_idx / 2); // index of slot [0-7]
    return slot_idx;
}

int vtem_control::VtemControl::get_single_pressure(const int valve_idx) {
    ensure_connection();

    int slot_idx = get_slot_idx_from_valve_idx(valve_idx);
    int slot_remain = valve_idx - 2*slot_idx; // either 0 or 1 for valve in slot
    ensure_motion_app(slot_idx, 3, 3);

    const auto dest = &input_buffer_[valve_idx];
    const auto addr = address_input_start + cpx_input_offset + 2*3*slot_idx + 1 + slot_remain;

    if (modbus_read_registers(ctx_, addr, 2, dest) == -1) {
        throw std::runtime_error("Failed to read CPX modbus register.");
    }

    return *dest;
}

void vtem_control::VtemControl::set_single_pressure(const int valve_idx, const int pressure = 0) {
    ensure_connection();

    int slot_idx = get_slot_idx_from_valve_idx(valve_idx);
    int slot_remain = valve_idx - 2*slot_idx; // either 0 or 1 for valve in slot
    ensure_motion_app(slot_idx, 3, 3);

    const auto addr = address_output_start + cpx_output_offset + 3*slot_idx + 1 + slot_remain;

    if (modbus_write_register(ctx_, addr, pressure) == -1) {
        throw std::runtime_error("Failed to write CPX modbus register.");
    }
}

void vtem_control::VtemControl::get_all_pressures(std::vector<int> *output) {
    ensure_connection();

    for (auto valve_idx = 0; valve_idx < 2*num_slots; valve_idx++) {
        get_single_pressure(valve_idx);
        output->at(valve_idx) = input_buffer_[valve_idx];
    }
}

void vtem_control::VtemControl::set_all_pressures(const std::vector<int> &pressures) {
    ensure_connection();

    for (auto valve_idx = 0; valve_idx < 2*num_slots; valve_idx++) {
        set_single_pressure(valve_idx, pressures[valve_idx]);
    }
}