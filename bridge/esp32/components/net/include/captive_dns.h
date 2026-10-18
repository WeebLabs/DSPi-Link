/*
 * captive_dns.h - answers every DNS question on the setup network with the
 * bridge's own address (bridge spec 9.2).
 *
 * A phone that joins the setup network asks DNS for a test address such as
 * captive.apple.com. Answering every name with the bridge makes the phone
 * fetch that test page from the bridge, see a redirect instead of what it
 * expected, and open the setup page by itself.
 *
 * Portable: no ESP-IDF headers. net.c runs the UDP server that uses it.
 */

#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

#include <stddef.h>
#include <stdint.h>

#define CAPTIVE_DNS_PORT 53

/*
 * Builds the answer to one DNS query. An A (or ANY) question gets `address`
 * with a 60 s lifetime; any other type gets a successful answer with no
 * records, so the phone falls back to IPv4. Returns the answer's length, or
 * 0 to ignore the packet: a response, not a standard query, more than one
 * question, or malformed.
 */
size_t captive_dns_answer(const uint8_t *query, size_t length, const uint8_t address[4],
                          uint8_t *answer, size_t capacity);

#endif /* CAPTIVE_DNS_H */
