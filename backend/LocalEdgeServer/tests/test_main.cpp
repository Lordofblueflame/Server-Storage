#include <iostream>

bool run_protocol_wire_tests();
bool run_hostindexer_frame_tests();
bool run_ingest_coordinator_tests();
bool run_hostindexer_client_integration_tests();

int main() {
    run_protocol_wire_tests();
    run_hostindexer_frame_tests();
    run_ingest_coordinator_tests();
    run_hostindexer_client_integration_tests();
    std::cout << "All LocalEdgeServer tests passed.\n";
    return 0;
}
