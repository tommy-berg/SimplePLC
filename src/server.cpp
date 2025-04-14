#include "server.h"
#include "modbus_handler.h"
#include <modbus.h>
#include <iostream>
#include <unistd.h>
#include "plc_logic.h"

int ModbusServer::run() {
    modbus_t* ctx = modbus_new_tcp("0.0.0.0", 502);
    if (!ctx) {
        std::cerr << "Failed to listen on 0.0.0.0 port 502" << std::endl;
        return 1;
    }

    modbus_set_slave(ctx, 1);
    mapping_ = modbus_mapping_new(255, 255, 255, 255); 
    if (!mapping_) {
        std::cerr << "Failed to allocate Modbus mapping: " << modbus_strerror(errno) << std::endl;
        modbus_free(ctx);
        return 1;
    }
    
    
    int listen_socket = modbus_tcp_listen(ctx, 1);
    PlcLogic::start(mapping_);
    PlcLogic::loadScript("plc_logic.lua");
    while (true) {
        int client_socket = modbus_tcp_accept(ctx, &listen_socket);
        if (client_socket == -1) continue;

        while (true) {
            uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
            int rc = modbus_receive(ctx, query);

            if (rc > 0) {
                uint8_t func = query[7];
                if (func == 0x11)
                {
                    ModbusHandler::send_report_slave_id(client_socket, ctx, query, rc);
                    std::cout << "[DEBUG] Received Report Slave ID command" << std::endl;
                }
                else if (func == 0x2B)
                {                    
                    ModbusHandler::send_read_device_id(client_socket, ctx, query, rc);
                    std::cout << "[DEBUG] Received Read Device ID command" << std::endl;
                }
                else
                    ModbusHandler::handle_standard_function(ctx, query, rc, mapping_);
                    std::cout << "[DEBUG] Standard function requested" << std::endl;

            } else {
                break;
            }
        }

        close(client_socket);
    }

    close(listen_socket);
    modbus_mapping_free(mapping_);
    modbus_free(ctx);
    PlcLogic::stop();
    std::cout << "[Info] Modbus server stopped\n";
    return 0;
}

modbus_mapping_t* ModbusServer::get_mapping() {
    return mapping_;
}