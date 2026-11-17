#include "bsp_obd_dsp/skyhub_client.h"
#include "app_obd_dsp/signal_store.h"
#include <string.h>
uint32_t esp_random(void) { return 12345; }
void skyhub_client_start(void) {}
bool skyhub_client_pause(void) { return true; }
bool skyhub_client_receive(const uint8_t mac[6], const uint8_t *data, size_t len)
{ (void)mac; return data && len>=2 && data[0]=='S' && data[1]=='H'; }
skyhub_status_t skyhub_test_status;
void skyhub_client_snapshot(skyhub_status_t *out) { *out=skyhub_test_status; }
bool skyhub_client_scan(void) { return true; }
unsigned skyhub_test_bind_calls;
uint8_t skyhub_test_bound_mac[6];
bool skyhub_client_bind(const uint8_t mac[6])
{
    ++skyhub_test_bind_calls;
    memcpy(skyhub_test_bound_mac, mac, sizeof(skyhub_test_bound_mac));
    return true;
}
bool skyhub_client_unbind(void) { return true; }
unsigned skyhub_test_writes;
uint8_t skyhub_test_mask;
int32_t skyhub_test_values[3];
void signal_store_hub_apply(uint8_t mask, const int32_t values[3])
{
    ++skyhub_test_writes; skyhub_test_mask=mask;
    if (values) memcpy(skyhub_test_values,values,sizeof(skyhub_test_values));
}
uint16_t signal_store_hub_obd_mask(void) { return 0; }

uint8_t signal_store_hub_snapshot(int32_t values[3]) { memset(values,0,3*sizeof(*values)); return 0; }
