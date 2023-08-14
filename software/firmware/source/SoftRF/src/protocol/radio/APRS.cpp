/*
 *
 * Protocol_APRS.cpp
 * Encoder for Automatic Packet Reporting System radio protocol
 * Copyright (C) 2023 Linar Yusupov
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>

#include <protocol.h>
#include <LibAPRSesp.h>
#include <pbuf.h>
#include <parse_aprs.h>

#include "../../../SoftRF.h"
#include "../../driver/RF.h"

const rf_proto_desc_t aprs_proto_desc = {
  "APRS",
  .type            = RF_PROTOCOL_APRS,
  .modulation_type = RF_MODULATION_TYPE_2FSK, /* Bell 202 AFSK */
  .preamble_type   = APRS_PREAMBLE_TYPE,
  .preamble_size   = APRS_PREAMBLE_SIZE,
  .syncword        = APRS_SYNCWORD,
  .syncword_size   = APRS_SYNCWORD_SIZE,
  .net_id          = 0x0000, /* not in use */
  .payload_type    = RF_PAYLOAD_DIRECT,
  .payload_size    = APRS_PAYLOAD_SIZE,
  .payload_offset  = 0,
  .crc_type        = APRS_CRC_TYPE,
  .crc_size        = APRS_CRC_SIZE,

  .bitrate         = RF_BITRATE_38400, /* 1200 */
  .deviation       = RF_FREQUENCY_DEVIATION_9_6KHZ, /* TBD */
  .whitening       = RF_WHITENING_NONE, /* TBD */
  .bandwidth       = RF_RX_BANDWIDTH_SS_50KHZ, /* TBD */

  .air_time        = APRS_AIR_TIME,

  .tm_type         = RF_TIMING_INTERVAL,
  .tx_interval_min = APRS_TX_INTERVAL_MIN,
  .tx_interval_max = APRS_TX_INTERVAL_MAX,
  .slot0           = {0, 0},
  .slot1           = {0, 0}
};

static const char *AprsIcon[16] = // Icons for various FLARM acftType's
{  "/z",  //  0 = ?
   "/'",  //  1 = (moto-)glider    (most frequent)
   "/'",  //  2 = tow plane        (often)
   "/X",  //  3 = helicopter       (often)
   "/g" , //  4 = parachute        (rare but seen - often mixed with drop plane)
   "\\^", //  5 = drop plane       (seen)
   "/g" , //  6 = hang-glider      (rare but seen)
   "/g" , //  7 = para-glider      (rare but seen)
   "\\^", //  8 = powered aircraft (often)
   "/^",  //  9 = jet aircraft     (rare but seen)
   "/z",  //  A = UFO              (people set for fun)
   "/O",  //  B = balloon          (seen once)
   "/O",  //  C = airship          (seen once)
   "/'",  //  D = UAV              (drones, can become very common)
   "/z",  //  E = ground support   (ground vehicles at airfields)
   "\\n"  //  F = static object    (ground relay ?)
};

extern AX25Msg Incoming_APRS_Packet;
extern char Outgoing_APRS_Data[160];

struct pbuf_t aprs;
ParseAPRS aprsParse;

int packet2Raw(String &tnc2, AX25Msg &Packet) {
  if (Packet.len < 5) return 0;

  tnc2 = String(Packet.src.call);

  if (Packet.src.ssid > 0) {
      tnc2 += String(F("-"));
      tnc2 += String(Packet.src.ssid);
  }

  tnc2 += String(F(">"));
  tnc2 += String(Packet.dst.call);

  if (Packet.dst.ssid > 0) {
      tnc2 += String(F("-"));
      tnc2 += String(Packet.dst.ssid);
  }

  for (int i = 0; i < Packet.rpt_count; i++) {
      tnc2 += String(",");
      tnc2 += String(Packet.rpt_list[i].call);
      if (Packet.rpt_list[i].ssid > 0) {
          tnc2 += String("-");
          tnc2 += String(Packet.rpt_list[i].ssid);
      }
      if (Packet.rpt_flags & (1 << i)) tnc2 += "*";
  }

  tnc2 += String(F(":"));
  tnc2 += String((const char *)Packet.info);
  tnc2 += String("\n");

  // #ifdef DEBUG_TNC
  //     Serial.printf("[%d] ", ++pkgTNC_count);
  //     Serial.print(tnc2);
  // #endif

  return tnc2.length();
}

bool aprs_decode(void *pkt, ufo_t *this_aircraft, ufo_t *fop) {

  String tnc2;
  packet2Raw(tnc2, Incoming_APRS_Packet);

  // Serial.println("APRS RX: " + tnc2);

  int start_val = tnc2.indexOf(">", 0);
  if (start_val > 3)
  {
    String src_call = tnc2.substring(0, start_val);
    memset(&aprs, 0, sizeof(pbuf_t));
    aprs.buf_len = 300;
    aprs.packet_len = tnc2.length();
    tnc2.toCharArray(&aprs.data[0], aprs.packet_len);
    int start_info = tnc2.indexOf(":", 0);
    int end_ssid = tnc2.indexOf(",", 0);
    int start_dst = tnc2.indexOf(">", 2);
    int start_dstssid = tnc2.indexOf("-", start_dst);
    if ((start_dstssid > start_dst) && (start_dstssid < start_dst + 10))
    {
        aprs.dstcall_end_or_ssid = &aprs.data[start_dstssid];
    }
    else
    {
        aprs.dstcall_end_or_ssid = &aprs.data[end_ssid];
    }
    aprs.info_start = &aprs.data[start_info + 1];
    aprs.dstname = &aprs.data[start_dst + 1];
    aprs.dstname_len = end_ssid - start_dst;
    aprs.dstcall_end = &aprs.data[end_ssid];
    aprs.srccall_end = &aprs.data[start_dst];

    if (aprsParse.parse_aprs(&aprs))
    {
#if 0 // ndef RASPBERRY_PI
      Serial.print("lat: "); Serial.println(aprs.lat);
      Serial.print("lon: "); Serial.println(aprs.lng);
      Serial.print("alt: "); Serial.println(aprs.altitude);
      Serial.print("crs: "); Serial.println(aprs.course);
      Serial.print("spd: "); Serial.println(aprs.speed);
      Serial.print("id:  "); Serial.println(aprs.ogn_id, HEX);
#endif

      fop->protocol  = RF_PROTOCOL_APRS;

      fop->addr      = aprs.ogn_id & 0x00FFFFFF;
      fop->latitude  = aprs.lat;
      fop->longitude = aprs.lng;
      fop->altitude  = (float) aprs.altitude;                   /* metres */
      fop->course    = (aprs.course == 360) ? 0 : (float) aprs.course;
      fop->speed     = (float) aprs.speed / _GPS_KMPH_PER_KNOT; /* knots  */
      fop->timestamp = (uint32_t) this_aircraft->timestamp;

      uint8_t XX         = (aprs.ogn_id >> 24) & 0xFF;
      fop->addr_type     = XX & 0x3;
      fop->aircraft_type = (XX >> 2) & 0xF;
      fop->no_track      = (XX >> 6) & 0x1;
      fop->stealth       = (XX >> 7) & 0x1;

      if (fop->addr) return true;
    }
  }

  return false;
}

static void nmea_lat(float lat, char *buf)
{
  int   lat_int = (int) lat;
  float lat_dec = lat - lat_int;

  sprintf(buf, "%02d%05.2f%c", abs(lat_int), fabsf(lat_dec * 60), lat < 0 ? 'S' : 'N');
}

static void nmea_lon(float lon, char *buf)
{
  int   lon_int = (int) lon;
  float lon_dec = lon - lon_int;

  sprintf(buf, "%03d%05.2f%c", abs(lon_int), fabsf(lon_dec * 60), lon < 0 ? 'W' : 'E');
}

size_t aprs_encode(void *pkt, ufo_t *this_aircraft) {

  uint32_t id = this_aircraft->addr & 0x00FFFFFF;

#if !defined(SOFTRF_ADDRESS)
  uint8_t addr_type = ADDR_TYPE_ANONYMOUS;
#else
  uint8_t addr_type = id == SOFTRF_ADDRESS ? ADDR_TYPE_ICAO : ADDR_TYPE_ANONYMOUS;
#endif

  uint8_t acft_type = this_aircraft->aircraft_type > AIRCRAFT_TYPE_STATIC ?
          AIRCRAFT_TYPE_UNKNOWN : this_aircraft->aircraft_type;

  int course_i   = (int)  this_aircraft->course;
  int altitude_i = (int) (this_aircraft->altitude * _GPS_FEET_PER_METER);

  uint32_t XX = (this_aircraft->stealth  << 7UL) |
                (this_aircraft->no_track << 6UL) |
                (acft_type               << 2UL) |
                (addr_type                     );

  float lat = this_aircraft->latitude;
  float lon = this_aircraft->longitude;
  int   lat_int = (int) lat;
  float lat_dec = lat - lat_int;
  int   lon_int = (int) lon;
  float lon_dec = lon - lon_int;

  snprintf(Outgoing_APRS_Data, sizeof(Outgoing_APRS_Data),
           "%06X>%s:"
           "/"
           "%02d%02d%02dh"
           "%02d%05.2f%c"
           "/"
           "%03d%05.2f%c"
           "'"
           "%03d/%03d/A=%06d !W00! id%08X +000fpm +0.0rot 7.8dB -1.6kHz gps8x3",
           id, "OGFLR",
           gnss.time.hour(), gnss.time.minute(), gnss.time.second(),
           abs(lat_int), fabsf(lat_dec * 60), lat < 0 ? 'S' : 'N',
           abs(lon_int), fabsf(lon_dec * 60), lon < 0 ? 'W' : 'E',
           (course_i == 0) ? 360 : course_i,
           (int) this_aircraft->speed,        /* knots */
           (altitude_i < 0) ? 0 : altitude_i, /* feet  */
           id | (XX << 24));

  strcpy((char *) pkt, "NOT IN USE");

  return aprs_proto_desc.payload_size;
}
