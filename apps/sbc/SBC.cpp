/*
 * $Id: SBC.cpp 1784 2010-04-15 13:01:00Z sayer $
 *
 * Copyright (C) 2010 Stefan Sayer
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* 
SBC - feature-wishlist

- SDP filter (reconstructed SDP)
- maximum call duration timer
- accounting (MySQL DB, cassandra DB)
- RTP forwarding mode (bridging)
- RTP transcoding mode (bridging)
- overload handling (parallel call to target thresholds)
- call distribution
- select profile on monitoring in-mem DB record
- fallback profile
- add headers

 */
#include "SBC.h"

#include "log.h"
#include "AmUtils.h"
#include "AmAudio.h"
#include "AmPlugIn.h"
#include "AmMediaProcessor.h"
#include "AmConfigReader.h"
#include "AmSessionContainer.h"
#include "AmSipHeaders.h"

#include "HeaderFilter.h"

using std::map;

string SBCFactory::user;
string SBCFactory::domain;
string SBCFactory::pwd;
AmConfigReader SBCFactory::cfg;
AmSessionEventHandlerFactory* SBCFactory::session_timer_fact = NULL;

EXPORT_SESSION_FACTORY(SBCFactory,MOD_NAME);


bool SBCCallProfile::readFromConfiguration(const string& name, const string profile_file_name) {
  if (cfg.loadFile(profile_file_name)) {
    ERROR("reading SBC call profile from '%s'\n", profile_file_name.c_str());
    return false;
  }

  string hf_type = cfg.getParameter("header_filter", "transparent");
  if (hf_type=="transparent")
    headerfilter = Transparent;
  else if (hf_type=="whitelist")
    headerfilter = Whitelist;
  else if (hf_type=="blacklist")
    headerfilter = Blacklist;
  else {
    ERROR("invalid header_filter mode '%s'\n", hf_type.c_str());
    return false;
  }
  
  vector<string> elems = explode(cfg.getParameter("header_list"), ",");
  for (vector<string>::iterator it=elems.begin(); it != elems.end(); it++)
    headerfilter_list.insert(*it);

  string mf_type = cfg.getParameter("message_filter", "transparent");
  if (mf_type=="transparent")
    messagefilter = Transparent;
  else if (mf_type=="whitelist")
    messagefilter = Whitelist;
  else if (hf_type=="blacklist")
    messagefilter = Blacklist;
  else {
    ERROR("invalid message_filter mode '%s'\n", mf_type.c_str());
    return false;
  }
  
  elems = explode(cfg.getParameter("message_list"), ",");
  for (vector<string>::iterator it=elems.begin(); it != elems.end(); it++)
    messagefilter_list.insert(*it);

  sst_enabled = cfg.getParameter("enable_session_timer", "no") == "yes";
  use_global_sst_config = !cfg.hasParameter("session_expires");
  
  auth_enabled = cfg.getParameter("enable_auth", "no") == "yes";
  auth_credentials.user = cfg.getParameter("auth_user");
  auth_credentials.pwd = cfg.getParameter("auth_pwd");

  ruri = cfg.getParameter("RURI");
  from = cfg.getParameter("From");
  to = cfg.getParameter("To");

  INFO("SBC: loaded SBC profile '%s':\n", name.c_str());

  INFO("SBC:      RURI = '%s'\n", ruri.c_str());
  INFO("SBC:      From = '%s'\n", from.c_str());
  INFO("SBC:      To   = '%s'\n", to.c_str());
  INFO("SBC:      header filter  is %s, %zd items in list\n",
       FilterType2String(headerfilter), headerfilter_list.size());
  INFO("SBC:      message filter is %s, %zd items in list\n",
       FilterType2String(messagefilter), messagefilter_list.size());
  INFO("SBC:      SST %sabled\n", sst_enabled?"en":"dis");
  INFO("SBC:      SIP auth %sabled\n", auth_enabled?"en":"dis");
  
  return true;
}

SBCFactory::SBCFactory(const string& _app_name)
: AmSessionFactory(_app_name)
{
}


int SBCFactory::onLoad()
{

  if(cfg.loadFile(AmConfig::ModConfigPath + string(MOD_NAME ".conf"))) {
    ERROR("No configuration for sbc present (%s)\n",
	 (AmConfig::ModConfigPath + string(MOD_NAME ".conf")).c_str()
	 );
    return -1;
  }

  session_timer_fact = AmPlugIn::instance()->getFactory4Seh("session_timer");
  if(!session_timer_fact) {
    ERROR("could not load session_timer from session_timer plug-in\n");
    return -1;
  }

  vector<string> profiles_names = explode(cfg.getParameter("profiles"), ",");
  for (vector<string>::iterator it =
	 profiles_names.begin(); it != profiles_names.end(); it++) {
    string profile_file_name = AmConfig::ModConfigPath + *it + ".sbcprofile.conf";
    if (!call_profiles[*it].readFromConfiguration(*it, profile_file_name)) {
      ERROR("configuring SBC call profile from '%s'\n", profile_file_name.c_str());
      return -1;
    }
  }

  active_profile = cfg.getParameter("active_profile");
  if (active_profile != "$(paramhdr)" &&
      active_profile != "$(ruri.user)" &&
      call_profiles.find(active_profile) == call_profiles.end()) {
    ERROR("call profile active_profile '%s' not loaded!\n", active_profile.c_str());
    return -1;
  }

  INFO("SBC: active profile: '%s'\n", active_profile.c_str());
  return 0;
}


AmSession* SBCFactory::onInvite(const AmSipRequest& req)
{
  string profile = active_profile;
  if (profile == "$(paramhdr)") {
    string app_param = getHeader(req.hdrs, PARAM_HDR, true);
    profile = get_header_keyvalue(app_param,"profile");
  } else if (profile == "$(ruri.user)") {
    profile = req.user;
  }

  map<string, SBCCallProfile>::iterator it=
    call_profiles.find(profile);
  if (it==call_profiles.end()) {
    ERROR("could not find call profile '%s' (active_profile = %s)\n",
	  profile.c_str(), active_profile.c_str());
    throw AmSession::Exception(500,"Server Internal Error");
  }

  DBG("using call profile '%s'\n", profile.c_str());
  SBCCallProfile& call_profile = it->second;
  AmConfigReader& sst_cfg = call_profile.use_global_sst_config ?
    cfg : call_profile.cfg; // override with profile config

  if (call_profile.sst_enabled) {
    DBG("Enabling SIP Session Timers\n");
    if (!session_timer_fact->onInvite(req, sst_cfg))
      return NULL;
  }

  SBCDialog* b2b_dlg = new SBCDialog(call_profile);

  if (call_profile.sst_enabled) {
    AmSessionEventHandler* h = session_timer_fact->getHandler(b2b_dlg);
    if(!h) {
      delete b2b_dlg;
      ERROR("could not get a session timer event handler\n");
      throw AmSession::Exception(500,"Server internal error");
    }

    if (h->configure(sst_cfg)){
      ERROR("Could not configure the session timer: disabling session timers.\n");
      delete h;
    } else {
      b2b_dlg->addHandler(h);
    }
  }

  return b2b_dlg;
}


SBCDialog::SBCDialog(const SBCCallProfile& call_profile) // AmDynInvoke* user_timer)
  : m_state(BB_Init),
    call_profile(call_profile)
{
  set_sip_relay_only(false);
}


SBCDialog::~SBCDialog()
{
}

void SBCDialog::replaceParsedParam(const string& s, size_t p,
				   AmUriParser& parsed, string& res) {
  switch (s[p+1]) {
  case 'u': { // URI
    res+=parsed.uri_user+"@"+parsed.uri_host;
    if (!parsed.uri_port.empty())
      res+=":"+parsed.uri_port;
  } break;
  case 'U': res+=parsed.uri_user; break; // User
  case 'd': { // domain
    res+=parsed.uri_host;
    if (!parsed.uri_port.empty())
      res+=":"+parsed.uri_port;
  } break;
  case 'h': res+=parsed.uri_host; break; // host
  case 'p': res+=parsed.uri_port; break; // port
  case 'H': res+=parsed.uri_headers; break; // Headers
  case 'P': res+=parsed.uri_param; break; // Params
  default: WARN("unknown replace pattern $%c%c\n",
		s[p], s[p+1]); break;
  };
}


string SBCDialog::replaceParameters(const char* r_type,
				    const string& s, const AmSipRequest& req,
				    const string& app_param,
				    AmUriParser& ruri_parser, AmUriParser& from_parser,
				    AmUriParser& to_parser) {
  string res;
  bool is_replaced = false;
  size_t p = 0;
  char last_char=' ';
  
  while (p<s.length()) {
    size_t skip_chars = 1;
    if (last_char=='\\') {
      res += s[p];
      is_replaced = true;
    } else if (s[p]=='\\') {
      if (p==s.length()-1)
      res += s[p];
    } else if ((s[p]=='$') && (s.length() >= p+1)) {
      is_replaced = true;
      p++;
      switch (s[p]) {
      case 'f': { // from
	if ((s.length() == p+1) || (s[p+1] == '.')) {
	  res += req.from;
	  break;
	}
	  
	if (from_parser.uri.empty()) {
	  from_parser.uri = req.from;
	  if (!from_parser.parse_uri()) {
	    WARN("Error parsing From URI '%s'\n", req.from.c_str());
	    break;
	  }
	}

	replaceParsedParam(s, p, from_parser, res);

      }; break;

      case 't': { // to
	if ((s.length() == p+1) || (s[p+1] == '.')) {
	  res += req.to;
	  break;
	}

	if (to_parser.uri.empty()) {
	  to_parser.uri = req.to;
	  if (!to_parser.parse_uri()) {
	    WARN("Error parsing To URI '%s'\n", req.to.c_str());
	    break;
	  }
	}

	replaceParsedParam(s, p, to_parser, res);

      }; break;

      case 'r': { // r-uri
	if ((s.length() == p+1) || (s[p+1] == '.')) {
	  res += req.r_uri;
	  break;
	}
	
	if (ruri_parser.uri.empty()) {
	  ruri_parser.uri = req.r_uri;
	  if (!ruri_parser.parse_uri()) {
	    WARN("Error parsing R-URI '%s'\n", req.r_uri.c_str());
	    break;
	  }
	}
	replaceParsedParam(s, p, ruri_parser, res);
      }; break;

#define case_HDR(pv_char, pv_name, hdr_name)				\
	case pv_char: {							\
	  AmUriParser uri_parser;					\
	  uri_parser.uri = getHeader(req.hdrs, hdr_name);		\
	  if ((s.length() == p+1) || (s[p+1] == '.')) {			\
	    res += uri_parser.uri;					\
	    break;							\
	  }								\
									\
	  if (!uri_parser.parse_uri()) {				\
	    WARN("Error parsing " pv_name " URI '%s'\n", uri_parser.uri.c_str()); \
	    break;							\
	  }								\
	  if (s[p+1] == 'i') {						\
	    res+=uri_parser.uri_user+"@"+uri_parser.uri_host;		\
	    if (!uri_parser.uri_port.empty())				\
	      res+=":"+uri_parser.uri_port;				\
	  } else {							\
	    replaceParsedParam(s, p, uri_parser, res);			\
	  }								\
	}; break;							

	case_HDR('a', "PAI", SIP_HDR_P_ASSERTED_IDENTITY);  // P-Asserted-Identity
	case_HDR('p', "PPI", SIP_HDR_P_PREFERRED_IDENTITY); // P-Preferred-Identity

      case 'P': { // app-params
	if (s[p+1] != '(') {
	  WARN("Error parsing P param replacement (missing '(')\n");
	  break;
	}
	if (s.length()<p+3) {
	  WARN("Error parsing P param replacement (short string)\n");
	  break;
	}

	size_t skip_p = p+2;
	for (;skip_p<s.length() && s[skip_p] != ')';skip_p++) { }
	if (skip_p==s.length()) {
	  WARN("Error parsing P param replacement (unclosed brackets)\n");
	  break;
	}
	string param_name = s.substr(p+2, skip_p-p-2);
	// DBG("param_name = '%s' (skip-p - p = %d)\n", param_name.c_str(), skip_p-p);
	res += get_header_keyvalue(app_param, param_name);
	skip_chars = skip_p-p;
      } break;

      default: {
	WARN("unknown replace pattern $%c%c\n",
	     s[p], s[p+1]);
      }; break;
      };

      p+=skip_chars; // skip $.X      
    } else {
      res += s[p];
    }

    last_char = s[p];
    p++;
  }

  if (is_replaced) {
    DBG("%s pattern replace: '%s' -> '%s'\n", r_type, s.c_str(), res.c_str());
  }
  return res;
}

void SBCDialog::onInvite(const AmSipRequest& req)
{
  AmUriParser ruri_parser, from_parser, to_parser;

  DBG("processing initial INVITE\n");

  if(dlg.reply(req, 100, "Connecting") != 0) {
    throw AmSession::Exception(500,"Failed to reply 100");
  }

  string app_param = getHeader(req.hdrs, PARAM_HDR, true);

  ruri = call_profile.ruri.empty() ? 
    req.r_uri : replaceParameters("RURI", call_profile.ruri, req, app_param,
				  ruri_parser, from_parser, to_parser);

  from = call_profile.from.empty() ? 
    req.from : replaceParameters("From", call_profile.from, req, app_param,
				  ruri_parser, from_parser, to_parser);

  to = call_profile.to.empty() ? 
    req.to : replaceParameters("To", call_profile.to, req, app_param,
				  ruri_parser, from_parser, to_parser);

  m_state = BB_Dialing;

  invite_req = req;

  removeHeader(invite_req.hdrs,PARAM_HDR);
  removeHeader(invite_req.hdrs,"P-App-Name");

  if (call_profile.sst_enabled) {
    removeHeader(invite_req.hdrs,SIP_HDR_SESSION_EXPIRES);
    removeHeader(invite_req.hdrs,SIP_HDR_MIN_SE);
  }

  inplaceHeaderFilter(invite_req.hdrs, 
		      call_profile.headerfilter_list, call_profile.headerfilter);

  if (call_profile.auth_enabled) {
    call_profile.auth_credentials.user = 
      replaceParameters("auth_user", call_profile.auth_credentials.user, req, app_param,
			ruri_parser, from_parser, to_parser);
    call_profile.auth_credentials.pwd = 
      replaceParameters("auth_pwd", call_profile.auth_credentials.pwd, req, app_param,
			ruri_parser, from_parser, to_parser);
  }

  DBG("SBC: connecting to <%s>\n",ruri.c_str());
  DBG("     From:  <%s>\n",from.c_str());
  DBG("     To:  <%s>\n",to.c_str());
  connectCallee(to, ruri, true);
}

void SBCDialog::process(AmEvent* ev)
{
  AmB2BCallerSession::process(ev);
}

void SBCDialog::relayEvent(AmEvent* ev) {
  if (call_profile.headerfilter != Transparent) {
    if (ev->event_id == B2BSipRequest) {
      B2BSipRequestEvent* req_ev = dynamic_cast<B2BSipRequestEvent*>(ev);
      assert(req_ev);
      inplaceHeaderFilter(req_ev->req.hdrs, 
			  call_profile.headerfilter_list, call_profile.headerfilter);
    } else if (ev->event_id == B2BSipReply) {
      B2BSipReplyEvent* reply_ev = dynamic_cast<B2BSipReplyEvent*>(ev);
      assert(reply_ev);
      inplaceHeaderFilter(reply_ev->reply.hdrs, 
			  call_profile.headerfilter_list, call_profile.headerfilter);
    }
  }

  AmB2BCallerSession::relayEvent(ev);
}

void SBCDialog::onSipRequest(const AmSipRequest& req) {
  // AmB2BSession does not call AmSession::onSipRequest for 
  // forwarded requests - so lets call event handlers here
  // todo: this is a hack, replace this by calling proper session 
  // event handler in AmB2BSession
  bool fwd = sip_relay_only &&
    (req.method != "BYE") &&
    (req.method != "CANCEL");
  if (fwd) {
      CALL_EVENT_H(onSipRequest,req);
  }

  if (fwd && call_profile.messagefilter != Transparent) {
    bool is_filtered = (call_profile.messagefilter == Whitelist) ^ 
      (call_profile.messagefilter_list.find(req.method) != 
       call_profile.messagefilter_list.end());
    if (is_filtered) {
      DBG("replying 405 to filtered message '%s'\n", req.method.c_str());
      dlg.reply(req, 405, "Method Not Allowed", "", "", "", SIP_FLAGS_VERBATIM);
      return;
    }
  }

  AmB2BCallerSession::onSipRequest(req);
}

void SBCDialog::onSipReply(const AmSipReply& reply, int old_dlg_status,
			      const string& trans_method)
{
  TransMap::iterator t = relayed_req.find(reply.cseq);
  bool fwd = t != relayed_req.end();

  DBG("onSipReply: %i %s (fwd=%i)\n",reply.code,reply.reason.c_str(),fwd);
  DBG("onSipReply: content-type = %s\n",reply.content_type.c_str());
  if (fwd) {
      CALL_EVENT_H(onSipReply,reply, old_dlg_status, trans_method);
  }

  AmB2BCallerSession::onSipReply(reply,old_dlg_status, trans_method);
}

bool SBCDialog::onOtherReply(const AmSipReply& reply)
{
  bool ret = false;

  if ((m_state == BB_Dialing) && (reply.cseq == invite_req.cseq)) {
    if (reply.code < 200) {
      DBG("Callee is trying... code %d\n", reply.code);
    }
    else if(reply.code < 300) {
      if(getCalleeStatus()  == Connected) {
        m_state = BB_Connected;
      }
    }
    else if(reply.code == 487 && dlg.getStatus() == AmSipDialog::Pending) {
      DBG("Stopping leg A on 487 from B with 487\n");
      dlg.reply(invite_req, 487, "Request terminated");
      setStopped();
      ret = true;
    }
    else if (reply.code >= 300 && dlg.getStatus() == AmSipDialog::Connected) {
      DBG("Callee final error in connected state with code %d\n",reply.code);
      terminateLeg();
    }
    else {
      DBG("Callee final error with code %d\n",reply.code);
      AmB2BCallerSession::onOtherReply(reply);
    }
  }
  return ret;
}


void SBCDialog::onOtherBye(const AmSipRequest& req)
{
//   stopAccounting();
  AmB2BCallerSession::onOtherBye(req);
}


void SBCDialog::onBye(const AmSipRequest& req)
{
  if (m_state == BB_Connected) {
//     stopAccounting();
  }
  terminateOtherLeg();
  setStopped();
}


void SBCDialog::onCancel()
{
  if(dlg.getStatus() == AmSipDialog::Pending) {
    DBG("Wait for leg B to terminate");
  } else {
    DBG("Canceling leg A on CANCEL since dialog is not pending");
    dlg.reply(invite_req, 487, "Request terminated");
    setStopped();
  }
}

void SBCDialog::createCalleeSession()
{
  SBCCalleeSession* callee_session = new SBCCalleeSession(this, call_profile);
  
  if (call_profile.auth_enabled) {
    // adding auth handler
    AmSessionEventHandlerFactory* uac_auth_f = 
      AmPlugIn::instance()->getFactory4Seh("uac_auth");
    if (NULL == uac_auth_f)  {
      INFO("uac_auth module not loaded. uac auth NOT enabled.\n");
    } else {
      AmSessionEventHandler* h = uac_auth_f->getHandler(callee_session);
      
      // we cannot use the generic AmSessionEventHandler hooks, 
      // because the hooks don't work in AmB2BSession
      callee_session->setAuthHandler(h);
      DBG("uac auth enabled for callee session.\n");
    }
  }

  if (call_profile.sst_enabled) {
    AmSessionEventHandler* h = SBCFactory::session_timer_fact->getHandler(callee_session);
    if(!h) {
      ERROR("could not get a session timer event handler\n");
      delete callee_session;
      throw AmSession::Exception(500,"Server internal error");
    }
    AmConfigReader& sst_cfg = call_profile.use_global_sst_config ? 
      SBCFactory::cfg: call_profile.cfg; // override with profile config

    if(h->configure(sst_cfg)){
      ERROR("Could not configure the session timer: disabling session timers.\n");
      delete h;
    } else {
      callee_session->addHandler(h);
    }
  }

  AmSipDialog& callee_dlg = callee_session->dlg;
  
  other_id = AmSession::getNewId();
  
  callee_dlg.local_tag    = other_id;
  callee_dlg.callid       = AmSession::getNewId() + "@" + AmConfig::LocalIP;
  
  // this will be overwritten by ConnectLeg event 
  callee_dlg.remote_party = to;
  callee_dlg.remote_uri   = ruri;

  callee_dlg.local_party  = from; 
  callee_dlg.local_uri    = from; 
  
  DBG("Created B2BUA callee leg, From: %s\n",
      from.c_str());

  if (AmConfig::LogSessions) {
    INFO("Starting B2B callee session %s app %s\n",
	 callee_session->getLocalTag().c_str(), invite_req.cmd.c_str());
  }

  MONITORING_LOG5(other_id.c_str(), 
		  "app",  invite_req.cmd.c_str(),
		  "dir",  "out",
		  "from", callee_dlg.local_party.c_str(),
		  "to",   callee_dlg.remote_party.c_str(),
		  "ruri", callee_dlg.remote_uri.c_str());

  callee_session->start();
  
  AmSessionContainer* sess_cont = AmSessionContainer::instance();
  sess_cont->addSession(other_id,callee_session);
}

SBCCalleeSession::SBCCalleeSession(const AmB2BCallerSession* caller,
				   const SBCCallProfile& call_profile) 
  : auth(NULL),
    call_profile(call_profile),
    AmB2BCalleeSession(caller) {
}

SBCCalleeSession::~SBCCalleeSession() {
  if (auth) 
    delete auth;
}

inline UACAuthCred* SBCCalleeSession::getCredentials() {
  return &call_profile.auth_credentials;
}

void SBCCalleeSession::relayEvent(AmEvent* ev) {
  if (call_profile.headerfilter != Transparent) {
    if (ev->event_id == B2BSipRequest) {
      B2BSipRequestEvent* req_ev = dynamic_cast<B2BSipRequestEvent*>(ev);
      assert(req_ev);
      inplaceHeaderFilter(req_ev->req.hdrs, 
			  call_profile.headerfilter_list, call_profile.headerfilter);
    } else if (ev->event_id == B2BSipReply) {
      B2BSipReplyEvent* reply_ev = dynamic_cast<B2BSipReplyEvent*>(ev);
      assert(reply_ev);
      inplaceHeaderFilter(reply_ev->reply.hdrs, 
			  call_profile.headerfilter_list, call_profile.headerfilter);
    }
  }

  AmB2BCalleeSession::relayEvent(ev);
}

void SBCCalleeSession::onSipRequest(const AmSipRequest& req) {
  // AmB2BSession does not call AmSession::onSipRequest for 
  // forwarded requests - so lets call event handlers here
  // todo: this is a hack, replace this by calling proper session 
  // event handler in AmB2BSession
  bool fwd = sip_relay_only &&
    (req.method != "BYE") &&
    (req.method != "CANCEL");
  if (fwd) {
      CALL_EVENT_H(onSipRequest,req);
  }

  if (fwd && call_profile.messagefilter != Transparent) {
    bool is_filtered = (call_profile.messagefilter == Whitelist) ^ 
      (call_profile.messagefilter_list.find(req.method) != 
       call_profile.messagefilter_list.end());
    if (is_filtered) {
      DBG("replying 405 to filtered message '%s'\n", req.method.c_str());
      dlg.reply(req, 405, "Method Not Allowed", "", "", "", SIP_FLAGS_VERBATIM);
      return;
    }
  }

  AmB2BCalleeSession::onSipRequest(req);
}

void SBCCalleeSession::onSipReply(const AmSipReply& reply, int old_dlg_status,
				     const string& trans_method)
{
  // call event handlers where it is not done 
  TransMap::iterator t = relayed_req.find(reply.cseq);
  bool fwd = t != relayed_req.end();
  DBG("onSipReply: %i %s (fwd=%i)\n",reply.code,reply.reason.c_str(),fwd);
  DBG("onSipReply: content-type = %s\n",reply.content_type.c_str());
  if(fwd) {
    CALL_EVENT_H(onSipReply,reply, old_dlg_status, trans_method);
  }

  if (NULL == auth) {    
    AmB2BCalleeSession::onSipReply(reply,old_dlg_status, trans_method);
    return;
  }
  
  unsigned int cseq_before = dlg.cseq;
  if (!auth->onSipReply(reply, old_dlg_status, trans_method)) {
      AmB2BCalleeSession::onSipReply(reply, old_dlg_status, trans_method);
  } else {
    if (cseq_before != dlg.cseq) {
      DBG("uac_auth consumed reply with cseq %d and resent with cseq %d; "
          "updating relayed_req map\n", 
	  reply.cseq, cseq_before);
      TransMap::iterator it=relayed_req.find(reply.cseq);
      if (it != relayed_req.end()) {
	relayed_req[cseq_before] = it->second;
	relayed_req.erase(it);
      }
    }
  }
}

void SBCCalleeSession::onSendRequest(const string& method, const string& content_type,
				     const string& body, string& hdrs, int flags, unsigned int cseq)
{
  if (NULL != auth) {
    DBG("auth->onSendRequest cseq = %d\n", cseq);
    auth->onSendRequest(method, content_type,
			body, hdrs, flags, cseq);
  }
  
  AmB2BCalleeSession::onSendRequest(method, content_type,
				     body, hdrs, flags, cseq);
}
