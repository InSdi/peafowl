/*
 * sip_extraction.c
 *
 * Given a .pcap file, extracts the SIP requestURI contained in it.
 *
 * Created on: 19/09/2012
 * =========================================================================
 * Copyright (c) 2016-2019 Daniele De Sensi (d.desensi.software@gmail.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * =========================================================================
 */

#include <peafowl/peafowl.h>
#include <pcap.h>
#include <net/ethernet.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>

int main(int argc, char** argv){
	if(argc!=2){
		fprintf(stderr, "Usage: %s pcap_file\n", argv[0]);
		return -1;
	}
	char* pcap_filename=argv[1];
	char errbuf[PCAP_ERRBUF_SIZE];

  pfwl_state_t* state=pfwl_init();
	pcap_t *handle=pcap_open_offline(pcap_filename, errbuf);

	if(handle==NULL){
		fprintf(stderr, "Couldn't open device %s: %s\n", pcap_filename, errbuf);
		return (2);
	}

	const u_char* packet;
	struct pcap_pkthdr header;

  pfwl_protocol_field_add(state, PFWL_FIELDS_SIP_REQUEST_URI);

	while((packet=pcap_next(handle, &header))!=NULL){
        pfwl_dissection_info_t r = pfwl_dissect_from_L2(state, packet, header.caplen, time(NULL), pcap_datalink(handle));

        if(r.protocol_l7 == PFWL_PROTOCOL_SIP &&
           r.protocol_fields[PFWL_FIELDS_SIP_REQUEST_URI].str.len){
          const char* field_value = r.protocol_fields[PFWL_FIELDS_SIP_REQUEST_URI].str.s;
          size_t field_len = r.protocol_fields[PFWL_FIELDS_SIP_REQUEST_URI].str.len;
          printf("Request URI detected: %.*s\n", (int) field_len, field_value);
        }
	}
	return 0;
}
