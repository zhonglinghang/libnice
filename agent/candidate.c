/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2006-2009 Collabora Ltd.
 *  Contact: Youness Alaoui
 * (C) 2006-2009 Nokia Corporation. All rights reserved.
 *  Contact: Kai Vehmanen
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Dafydd Harries, Collabora Ltd.
 *   Youness Alaoui, Collabora Ltd.
 *   Kai Vehmanen, Nokia
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */

/*
 * @file candidate.c
 * @brief ICE candidate functions
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#else
#define NICEAPI_EXPORT
#endif

#include <string.h>

#include "agent.h"
#include "component.h"
#include "interfaces.h"
#include "candidate-priv.h"

G_DEFINE_BOXED_TYPE (NiceCandidate, nice_candidate, nice_candidate_copy,
    nice_candidate_free);

/* (ICE 4.1.1 "Gathering Candidates") ""Every candidate is a transport
 * address. It also has a type and a base. Three types are defined and 
 * gathered by this specification - host candidates, server reflexive 
 * candidates, and relayed candidates."" (ID-19) */

NICEAPI_EXPORT NiceCandidate *
nice_candidate_new (NiceCandidateType type)
{
  NiceCandidateImpl *c;

  c = g_slice_new0 (NiceCandidateImpl);
  c->c.type = type;
  return (NiceCandidate *) c;
}

NICEAPI_EXPORT void
nice_candidate_free (NiceCandidate *candidate)
{
  NiceCandidateImpl *c = (NiceCandidateImpl *) candidate;
  /* better way of checking if socket is allocated? */

  if (candidate->username)
    g_free (candidate->username);

  if (candidate->password)
    g_free (candidate->password);

  if (c->turn)
    turn_server_unref (c->turn);

  if (c->stun_server)
    nice_address_free (c->stun_server);

  g_slice_free (NiceCandidateImpl, c);
}


guint32
nice_candidate_jingle_priority (NiceCandidate *candidate)
{
  switch (candidate->type)
    {
    case NICE_CANDIDATE_TYPE_HOST:             return 1000;
    case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE: return 900;
    case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:   return 900;
    case NICE_CANDIDATE_TYPE_RELAYED:          return 500;
    default:                                   return 0;
    }
}

guint32
nice_candidate_msn_priority (NiceCandidate *candidate)
{
  switch (candidate->type)
    {
    case NICE_CANDIDATE_TYPE_HOST:             return 830;
    case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE: return 550;
    case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:   return 550;
    case NICE_CANDIDATE_TYPE_RELAYED:          return 450;
    default:                                   return 0;
    }
}


/*
 * ICE 4.1.2.1. "Recommended Formula" (ID-19):
 * returns number between 1 and 0x7effffff 
 */
guint32
nice_candidate_ice_priority_full (
  // must be ∈ (0, 126) (max 2^7 - 2)
  guint type_preference,
  // must be ∈ (0, 65535) (max 2^16 - 1)
  guint local_preference,
  // must be ∈ (0, 255) (max 2 ^ 8 - 1)
  guint component_id)
{
  return (
      0x1000000 * type_preference +
      0x100 * local_preference +
      (0x100 - component_id));
}

static guint16
nice_candidate_ice_local_preference_full (guint direction_preference,
    guint turn_preference, guint other_preference)
{
  /*
   * bits  0- 5: other_preference (ip local preference)
   *       6- 8: turn_preference
   *       9-12: <unused>
   *      13-15: direction_preference
   */
  g_assert (other_preference < NICE_CANDIDATE_MAX_LOCAL_ADDRESSES);
  g_assert (turn_preference < NICE_CANDIDATE_MAX_TURN_SERVERS);
  g_assert (direction_preference < 8);

  return (direction_preference << 13) +
      (turn_preference << 6) +
      other_preference;
}

static guint
nice_candidate_ip_local_preference (const NiceCandidate *candidate)
{
  guint preference = 0;
  gchar ip_string[INET6_ADDRSTRLEN];
  GList/*<owned gchar*>*/ *ips = NULL;
  GList/*<unowned gchar*>*/ *iter;

  /* Ensure otherwise identical host candidates with only different IP addresses
   * (multihomed host) get assigned different priorities. Position of the IP in
   * the list obtained from nice_interfaces_get_local_ips() serves here as the
   * distinguishing value of other_preference. Reflexive and relayed candidates
   * are likewise differentiated by their base address.
   *
   * This is required by RFC 5245 Section 4.1.2.1:
   *   https://tools.ietf.org/html/rfc5245#section-4.1.2.1
   */
  if (candidate->type == NICE_CANDIDATE_TYPE_HOST) {
    nice_address_to_string (&candidate->addr, ip_string);
  } else {
    nice_address_to_string (&candidate->base_addr, ip_string);
  }

  ips = nice_interfaces_get_local_ips (TRUE);

  for (iter = ips; iter; iter = g_list_next (iter)) {
    /* Strip the IPv6 link-local scope string */
    gchar **tokens = g_strsplit (iter->data, "%", 2);
    gboolean match = (g_strcmp0 (ip_string, tokens[0]) == 0);
    g_strfreev (tokens);
    if (match)
      break;
    ++preference;
  }

  g_list_free_full (ips, g_free);

  return preference;
}

static guint16
nice_candidate_ice_local_preference (const NiceCandidate *candidate)
{
  const NiceCandidateImpl *c = (NiceCandidateImpl *) candidate;
  guint direction_preference = 0;
  guint turn_preference = 0;

  switch (candidate->transport)
    {
      case NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
        if (candidate->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE ||
            candidate->type == NICE_CANDIDATE_TYPE_HOST)
          direction_preference = 4;
        else
          direction_preference = 6;
        break;
      case NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
        if (candidate->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE ||
            candidate->type == NICE_CANDIDATE_TYPE_HOST)
          direction_preference = 2;
        else
          direction_preference = 4;
        break;
      case NICE_CANDIDATE_TRANSPORT_TCP_SO:
        if (candidate->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE ||
            candidate->type == NICE_CANDIDATE_TYPE_HOST)
          direction_preference = 6;
        else
          direction_preference = 2;
        break;
      case NICE_CANDIDATE_TRANSPORT_UDP:
      default:
        direction_preference = 1;
        break;
    }

  /* Relay candidates are assigned a unique local preference at
   * creation time.
   */
  if (candidate->type == NICE_CANDIDATE_TYPE_RELAYED) {
    g_assert (c->turn);
    turn_preference = c->turn->preference;
  }

  return nice_candidate_ice_local_preference_full (direction_preference,
      turn_preference, nice_candidate_ip_local_preference (candidate));
}

static guint16
nice_candidate_ms_ice_local_preference_full (guint transport_preference,
    guint direction_preference, guint turn_preference, guint other_preference)
{
  /*
   * bits 0- 5: other_preference (ip local preference)
   *      6- 8: turn_preference
   *      9-11: direction_preference
   *     12-15: transport_preference
   */
  g_assert (other_preference < NICE_CANDIDATE_MAX_LOCAL_ADDRESSES);
  g_assert (turn_preference < NICE_CANDIDATE_MAX_TURN_SERVERS);
  g_assert (direction_preference < 8);
  g_assert (transport_preference < 16);

  return (transport_preference << 12) +
      (direction_preference << 9) +
      (turn_preference << 6) +
      other_preference;
}

static guint32
nice_candidate_ms_ice_local_preference (const NiceCandidate *candidate)
{
  const NiceCandidateImpl *c = (NiceCandidateImpl *) candidate;
  guint transport_preference = 0;
  guint direction_preference = 0;
  guint turn_preference = 0;

  switch (candidate->transport)
    {
    case NICE_CANDIDATE_TRANSPORT_TCP_SO:
    case NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
      transport_preference = NICE_CANDIDATE_TRANSPORT_MS_PREF_TCP;
      direction_preference = NICE_CANDIDATE_DIRECTION_MS_PREF_ACTIVE;
      break;
    case NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
      transport_preference = NICE_CANDIDATE_TRANSPORT_MS_PREF_TCP;
      direction_preference = NICE_CANDIDATE_DIRECTION_MS_PREF_PASSIVE;
      break;
    case NICE_CANDIDATE_TRANSPORT_UDP:
    default:
      transport_preference = NICE_CANDIDATE_TRANSPORT_MS_PREF_UDP;
      break;
    }

  /* Relay candidates are assigned a unique local preference at
   * creation time.
   */
  if (candidate->type == NICE_CANDIDATE_TYPE_RELAYED) {
    g_assert (c->turn);
    turn_preference = c->turn->preference;
  }

  return nice_candidate_ms_ice_local_preference_full(transport_preference,
      direction_preference, turn_preference,
      nice_candidate_ip_local_preference (candidate));
}

static guint8
nice_candidate_ice_type_preference (const NiceCandidate *candidate,
    gboolean reliable, gboolean nat_assisted)
{
  const NiceCandidateImpl *c = (NiceCandidateImpl *) candidate;
  guint8 type_preference;

  switch (candidate->type)
    {
    case NICE_CANDIDATE_TYPE_HOST:
      type_preference = NICE_CANDIDATE_TYPE_PREF_HOST;
      break;
    case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
      type_preference = NICE_CANDIDATE_TYPE_PREF_PEER_REFLEXIVE;
      break;
    case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
      if (nat_assisted)
        type_preference = NICE_CANDIDATE_TYPE_PREF_NAT_ASSISTED;
      else
        type_preference = NICE_CANDIDATE_TYPE_PREF_SERVER_REFLEXIVE;
      break;
    case NICE_CANDIDATE_TYPE_RELAYED:
      if (c->turn->type == NICE_RELAY_TYPE_TURN_UDP)
        type_preference = NICE_CANDIDATE_TYPE_PREF_RELAYED_UDP;
      else
        type_preference = NICE_CANDIDATE_TYPE_PREF_RELAYED;
      break;
    default:
      type_preference = 0;
      break;
    }

  if ((reliable && candidate->transport == NICE_CANDIDATE_TRANSPORT_UDP) ||
      (!reliable && candidate->transport != NICE_CANDIDATE_TRANSPORT_UDP)) {
    type_preference = type_preference / 2;
  }

  return type_preference;
}

guint32
nice_candidate_ice_priority (const NiceCandidate *candidate,
    gboolean reliable, gboolean nat_assisted)
{
  guint8 type_preference;
  guint16 local_preference;

  type_preference = nice_candidate_ice_type_preference (candidate, reliable,
      nat_assisted);
  local_preference = nice_candidate_ice_local_preference (candidate);

  return nice_candidate_ice_priority_full (type_preference, local_preference,
      candidate->component_id);
}

guint32
nice_candidate_ms_ice_priority (const NiceCandidate *candidate,
    gboolean reliable, gboolean nat_assisted)
{
  guint8 type_preference;
  guint16 local_preference;

  type_preference = nice_candidate_ice_type_preference (candidate, reliable,
      nat_assisted);
  local_preference = nice_candidate_ms_ice_local_preference (candidate);

  return nice_candidate_ice_priority_full (type_preference, local_preference,
      candidate->component_id);
}

/*
 * Calculates the pair priority as specified in ICE
 * sect 5.7.2. "Computing Pair Priority and Ordering Pairs" (ID-19).
 */
guint64
nice_candidate_pair_priority (guint32 o_prio, guint32 a_prio)
{
  guint32 max = o_prio > a_prio ? o_prio : a_prio;
  guint32 min = o_prio < a_prio ? o_prio : a_prio;
  /* These two constants are here explictly to make some version of GCC happy */
  const guint64 one = 1;
  const guint64 thirtytwo = 32;

  return (one << thirtytwo) * min + 2 * max + (o_prio > a_prio ? 1 : 0);
}

void
nice_candidate_pair_priority_to_string (guint64 prio, gchar *string)
{
  g_snprintf (string, NICE_CANDIDATE_PAIR_PRIORITY_MAX_SIZE,
      "%08" G_GINT64_MODIFIER "x:%08" G_GINT64_MODIFIER "x:%" G_GUINT64_FORMAT,
      prio >> 32, (prio >> 1) & 0x7fffffff, prio & 1);
}

/*
 * Copies a candidate
 */
NICEAPI_EXPORT NiceCandidate *
nice_candidate_copy (const NiceCandidate *candidate)
{
  NiceCandidateImpl *copy;

  g_return_val_if_fail (candidate != NULL, NULL);

  copy = (NiceCandidateImpl *) nice_candidate_new (candidate->type);
  memcpy (copy, candidate, sizeof(NiceCandidateImpl));

  copy->turn = NULL;
  copy->c.username = g_strdup (copy->c.username);
  copy->c.password = g_strdup (copy->c.password);
  if (copy->stun_server)
    copy->stun_server = nice_address_dup (copy->stun_server);

  return (NiceCandidate *) copy;
}

NICEAPI_EXPORT gboolean
nice_candidate_equal_target (const NiceCandidate *candidate1,
    const NiceCandidate *candidate2)
{
  g_return_val_if_fail (candidate1 != NULL, FALSE);
  g_return_val_if_fail (candidate2 != NULL, FALSE);

  return (candidate1->transport == candidate2->transport &&
      nice_address_equal (&candidate1->addr, &candidate2->addr));
}


NICEAPI_EXPORT const gchar *
nice_candidate_type_to_string (NiceCandidateType type)
{
  switch (type) {
    case NICE_CANDIDATE_TYPE_HOST:
      return "host";
    case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
      return "srflx";
    case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
      return "prflx";
    case NICE_CANDIDATE_TYPE_RELAYED:
      return "relay";
    default:
      g_assert_not_reached ();
  }
}

NICEAPI_EXPORT const gchar *
nice_candidate_transport_to_string (NiceCandidateTransport transport)
{
  switch (transport) {
    case NICE_CANDIDATE_TRANSPORT_UDP:
      return "udp";
    case NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
      return "tcp-act";
    case NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
      return "tcp-pass";
    case NICE_CANDIDATE_TRANSPORT_TCP_SO:
      return "tcp-so";
    default:
      g_assert_not_reached ();
  }
}

NICEAPI_EXPORT void
nice_candidate_relay_address (const NiceCandidate *candidate, NiceAddress *addr)
{
  const NiceCandidateImpl *c = (NiceCandidateImpl *) candidate;

  g_return_if_fail (candidate != NULL);
  g_return_if_fail (candidate->type != NICE_CANDIDATE_TYPE_RELAYED);

  *addr = c->turn->server;
}

NICEAPI_EXPORT gboolean
nice_candidate_stun_server_address (const NiceCandidate *candidate, NiceAddress *addr)
{
  const NiceCandidateImpl *c = (NiceCandidateImpl *) candidate;

  g_return_val_if_fail (candidate != NULL, FALSE);
  g_return_val_if_fail (candidate->type != NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE, FALSE);

  if (c->stun_server) {
    *addr = *c->stun_server;
    return TRUE;
  } else {
    return FALSE;
  }
}
