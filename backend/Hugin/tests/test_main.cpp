#include <iostream>

bool run_protocol_wire_tests();
bool run_muninn_frame_tests();
bool run_ingest_coordinator_tests();
bool run_muninn_client_integration_tests();
bool run_jwt_auth_tests();
bool run_frontend_dto_tests();

int main() {
    run_protocol_wire_tests();
    run_muninn_frame_tests();
    run_ingest_coordinator_tests();
    run_muninn_client_integration_tests();
    run_jwt_auth_tests();
    run_frontend_dto_tests();
    std::cout << "All Hugin tests passed.\n";
    return 0;
}
