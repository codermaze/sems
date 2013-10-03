#include <pcrecpp.h>
using namespace pcrecpp;

#include "SW_Vsc.h"
#include "AmConfig.h"
#include "AmUtils.h"
#include "AmPlugIn.h"

#include "sems.h"
#include "log.h"

//#include <my_global.h>
//#include <m_string.h>
#include <mysql.h>

#define MOD_NAME "sw_vsc"
#define SW_VSC_DATABASE "provisioning"

#define SW_VSC_GET_ATTRIBUTE_ID "select id from voip_preferences where attribute='%s'"
#define SW_VSC_GET_SUBSCRIBER_ID "select s.id, d.domain, d.id "\
	"from voip_subscribers s, voip_domains d "\
	"where s.uuid='%s' and s.domain_id = d.id"
#define SW_VSC_GET_PREFERENCE_ID "select id,value from voip_usr_preferences where subscriber_id=%llu " \
	"and attribute_id=%llu"
#define SW_VSC_DELETE_PREFERENCE_ID "delete from voip_usr_preferences where id=%llu"
#define SW_VSC_INSERT_PREFERENCE "insert into voip_usr_preferences (subscriber_id, attribute_id, value) "\
	"values(%llu, %llu, '%s')"
#define SW_VSC_UPDATE_PREFERENCE_ID "update voip_usr_preferences set value='%s' where id=%llu"
#define SW_VSC_INSERT_SPEEDDIAL "replace into voip_speed_dial (subscriber_id, slot, destination) "\
	"values(%llu, '%s', '%s')"
#define SW_VSC_INSERT_REMINDER "replace into voip_reminder (subscriber_id, time, recur) "\
	"values(%llu, '%s', '%s')"
#define SW_VSC_DELETE_REMINDER "delete from voip_reminder where subscriber_id=%llu"
#define SW_VSC_GET_USER_CALLEE_REWRITE_DPID "select value from " \
	"voip_usr_preferences vup, voip_preferences vp where " \
	"vup.subscriber_id=%llu and vp.attribute='rewrite_callee_in_dpid' " \
	"and vup.attribute_id = vp.id"
#define SW_VSC_GET_USER_CALLEE_REWRITES \
	"select vrr.match_pattern, vrr.replace_pattern from " \
	"voip_rewrite_rules vrr, voip_rewrite_rule_sets vrrs " \
	"where vrrs.callee_in_dpid=%llu and vrr.set_id = vrrs.id " \
	"and vrr.direction='in' and vrr.field='callee' " \
	"order by vrr.priority asc"
#define SW_VSC_GET_DOMAIN_CALLEE_REWRITES \
	"select vrr.match_pattern, vrr.replace_pattern " \
	"from voip_rewrite_rules vrr, voip_rewrite_rule_sets vrrs, " \
	"voip_preferences vp, voip_dom_preferences vdp " \
	"where vdp.domain_id=%llu and vp.attribute='rewrite_callee_in_dpid' " \
	"and vdp.attribute_id = vp.id and  vdp.value = vrrs.callee_in_dpid " \
	"and vrr.set_id = vrrs.id and vrr.direction='in' and " \
	"vrr.field='callee' order by vrr.priority asc"

#define SW_VSC_GET_VOICEMAIL_NUMBER "select mailbox from kamailio.voicemail_users "\
	"where customer_id='%s'"

#define SW_VSC_DELETE_VSC_DESTSET "delete from voip_cf_destination_sets where "\
	"subscriber_id=%llu and name='%s'"
#define SW_VSC_CREATE_VSC_DESTSET "insert into voip_cf_destination_sets (subscriber_id, name) "\
	"values(%llu, '%s')"
#define SW_VSC_CREATE_VSC_DEST "insert into voip_cf_destinations (destination_set_id, destination, priority) "\
	"values(%llu, '%s', %d)"
#define SW_VSC_DELETE_VSC_CFMAP "delete from voip_cf_mappings where subscriber_id=%llu and type='%s'"
#define SW_VSC_CREATE_VSC_CFMAP "insert into voip_cf_mappings "\
	"(subscriber_id, type, destination_set_id, time_set_id) "\
	"values(%llu, '%s', %llu, NULL)"

#define SW_VSC_DESTSET_CFU  "cfu_by_vsc"
#define SW_VSC_DESTSET_CFB  "cfb_by_vsc"
#define SW_VSC_DESTSET_CFT  "cft_by_vsc"
#define SW_VSC_DESTSET_CFNA "cfna_by_vsc"

EXPORT_SESSION_FACTORY(SW_VscFactory, MOD_NAME);

SW_VscFactory::SW_VscFactory(const string& _app_name)
  : AmSessionFactory(_app_name)
{
}

SW_VscFactory::~SW_VscFactory()
{
	regfree(&m_patterns.cfuOnPattern);
	regfree(&m_patterns.cfuOffPattern);
	regfree(&m_patterns.cfbOnPattern);
	regfree(&m_patterns.cfbOffPattern);
	regfree(&m_patterns.cftOnPattern);
	regfree(&m_patterns.cftOffPattern);
	regfree(&m_patterns.cfnaOnPattern);
	regfree(&m_patterns.cfnaOffPattern);
	regfree(&m_patterns.speedDialPattern);
	regfree(&m_patterns.reminderOnPattern);
	regfree(&m_patterns.reminderOffPattern);
}

int SW_VscFactory::onLoad()
{
  string aPath;
  string cfuOnPattern;
  string cfuOffPattern;
  string cfbOnPattern;
  string cfbOffPattern;
  string cftOnPattern;
  string cftOffPattern;
  string cfnaOnPattern;
  string cfnaOffPattern;
  string speedDialPattern;
  string reminderOnPattern;
  string reminderOffPattern;

  AmConfigReader cfg;
  if(cfg.loadFile(AmConfig::ModConfigPath + string(MOD_NAME ".conf")))
    return -1;

  configureModule(cfg);


  m_patterns.mysqlHost = cfg.getParameter("mysql_host", "");
  if(m_patterns.mysqlHost.empty())
  {
    ERROR("MysqlHost is empty\n");
    return -1;
  }
  m_patterns.mysqlPort = cfg.getParameterInt("mysql_port", 0);
  if(m_patterns.mysqlPort < 0 || m_patterns.mysqlPort > 65535)
  {
    ERROR("MysqlPort is invalid\n");
    return -1;
  }
  m_patterns.mysqlUser = cfg.getParameter("mysql_user", "");
  if(m_patterns.mysqlUser.empty())
  {
    ERROR("MysqlUser is empty\n");
    return -1;
  }
  m_patterns.mysqlPass = cfg.getParameter("mysql_pass", "");
  if(m_patterns.mysqlPass.empty())
  {
    ERROR("MysqlPass is empty\n");
    return -1;
  }




  aPath = cfg.getParameter("announce_path", ANNOUNCE_PATH);
  if(!aPath.empty() 
      && aPath[aPath.length()-1] != '/' )
    aPath += "/";
  
  m_patterns.failAnnouncement = aPath + cfg.getParameter("error_announcement", "");
  if(!file_exists(m_patterns.failAnnouncement)){
    ERROR("ErrorAnnouncement file does not exist ('%s').\n",
	  m_patterns.failAnnouncement.c_str());
    return -1;
  }
  m_patterns.unknownAnnouncement = aPath + cfg.getParameter("unknown_announcement", "");
  if(!file_exists(m_patterns.unknownAnnouncement)){
    ERROR("UnknownAnnouncement file does not exist ('%s').\n",
	  m_patterns.unknownAnnouncement.c_str());
    return -1;
  }
  m_patterns.voicemailNumber = cfg.getParameter("voicemail_number", "");
  if(m_patterns.voicemailNumber.empty())
  {
    ERROR("voicemailNumber is empty\n");
    return -1;
  }

  m_patterns.cfuOnAnnouncement = aPath + "/" + cfg.getParameter("cfu_on_announcement", "");
  if(!file_exists(m_patterns.cfuOnAnnouncement)){
    ERROR("CfuOnAnnouncement file does not exist ('%s').\n",
	  m_patterns.cfuOnAnnouncement.c_str());
    return -1;
  }
  cfuOnPattern = cfg.getParameter("cfu_on_pattern", "");
  if(cfuOnPattern.empty())
  {
    ERROR("CfuOnPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.cfuOnPattern, cfuOnPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("CfuOnPattern failed to compile ('%s'): %s\n",
	  cfuOnPattern.c_str(),
          strerror(errno));
    return -1;
  }

  m_patterns.cfuOffAnnouncement = aPath + "/" + cfg.getParameter("cfu_off_announcement", "");
  if(!file_exists(m_patterns.cfuOffAnnouncement)){
    ERROR("CfuOffAnnouncement file does not exist ('%s').\n",
	  m_patterns.cfuOffAnnouncement.c_str());
    return -1;
  }
  cfuOffPattern = cfg.getParameter("cfu_off_pattern", "");
  if(cfuOffPattern.empty())
  {
    ERROR("CfuOffPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.cfuOffPattern, cfuOffPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("CfuOffPattern failed to compile ('%s'): %s\n",
	  cfuOffPattern.c_str(),
          strerror(errno));
    return -1;
  }

  m_patterns.cfbOnAnnouncement = aPath + "/" + cfg.getParameter("cfb_on_announcement", "");
  if(!file_exists(m_patterns.cfbOnAnnouncement)){
    ERROR("CfbOnAnnouncement file does not exist ('%s').\n",
	  m_patterns.cfbOnAnnouncement.c_str());
    return -1;
  }
  cfbOnPattern = cfg.getParameter("cfb_on_pattern", "");
  if(cfbOnPattern.empty())
  {
    ERROR("CfbOnPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.cfbOnPattern, cfbOnPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("CfbOnPattern failed to compile ('%s'): %s\n",
	  cfbOnPattern.c_str(),
          strerror(errno));
    return -1;
  }

  m_patterns.cfbOffAnnouncement = aPath + "/" + cfg.getParameter("cfb_off_announcement", "");
  if(!file_exists(m_patterns.cfbOffAnnouncement)){
    ERROR("CfbOffAnnouncement file does not exist ('%s').\n",
	  m_patterns.cfbOffAnnouncement.c_str());
    return -1;
  }
  cfbOffPattern = cfg.getParameter("cfb_off_pattern", "");
  if(cfbOffPattern.empty())
  {
    ERROR("CfbOffPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.cfbOffPattern, cfbOffPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("CfbOffPattern failed to compile ('%s'): %s\n",
	  cfbOffPattern.c_str(),
          strerror(errno));
    return -1;
  }


  m_patterns.cftOnAnnouncement = aPath + "/" + cfg.getParameter("cft_on_announcement", "");
  if(!file_exists(m_patterns.cftOnAnnouncement)){
    ERROR("CftOnAnnouncement file does not exist ('%s').\n",
	  m_patterns.cftOnAnnouncement.c_str());
    return -1;
  }
  cftOnPattern = cfg.getParameter("cft_on_pattern", "");
  if(cftOnPattern.empty())
  {
    ERROR("CftOnPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.cftOnPattern, cftOnPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("CftOnPattern failed to compile ('%s'): %s\n",
	  cftOnPattern.c_str(),
          strerror(errno));
    return -1;
  }

  m_patterns.cftOffAnnouncement = aPath + "/" + cfg.getParameter("cft_off_announcement", "");
  if(!file_exists(m_patterns.cftOffAnnouncement)){
    ERROR("CftOffAnnouncement file does not exist ('%s').\n",
	  m_patterns.cftOffAnnouncement.c_str());
    return -1;
  }
  cftOffPattern = cfg.getParameter("cft_off_pattern", "");
  if(cftOffPattern.empty())
  {
    ERROR("CftOffPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.cftOffPattern, cftOffPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("CftOffPattern failed to compile ('%s'): %s\n",
	  cftOffPattern.c_str(),
          strerror(errno));
    return -1;
  }

  m_patterns.cfnaOnAnnouncement = aPath + "/" + cfg.getParameter("cfna_on_announcement", "");
  if(!file_exists(m_patterns.cfnaOnAnnouncement)){
    ERROR("CfnaOnAnnouncement file does not exist ('%s').\n",
	  m_patterns.cfnaOnAnnouncement.c_str());
    return -1;
  }
  cfnaOnPattern = cfg.getParameter("cfna_on_pattern", "");
  if(cfnaOnPattern.empty())
  {
    ERROR("CfnaOnPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.cfnaOnPattern, cfnaOnPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("CfnaOnPattern failed to compile ('%s'): %s\n",
	  cfnaOnPattern.c_str(),
          strerror(errno));
    return -1;
  }

  m_patterns.cfnaOffAnnouncement = aPath + "/" + cfg.getParameter("cfna_off_announcement", "");
  if(!file_exists(m_patterns.cfnaOffAnnouncement)){
    ERROR("CfnaOffAnnouncement file does not exist ('%s').\n",
	  m_patterns.cfnaOffAnnouncement.c_str());
    return -1;
  }
  cfnaOffPattern = cfg.getParameter("cfna_off_pattern", "");
  if(cfnaOffPattern.empty())
  {
    ERROR("CfnaOffPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.cfnaOffPattern, cfnaOffPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("CfnaOffPattern failed to compile ('%s'): %s\n",
	  cfnaOffPattern.c_str(),
          strerror(errno));
    return -1;
  }

  m_patterns.speedDialAnnouncement = aPath + "/" + cfg.getParameter("speed_dial_announcement", "");
  if(!file_exists(m_patterns.speedDialAnnouncement)){
    ERROR("SpeedDialAnnouncement file does not exist ('%s').\n",
	  m_patterns.speedDialAnnouncement.c_str());
    return -1;
  }
  speedDialPattern = cfg.getParameter("speed_dial_pattern", "");
  if(speedDialPattern.empty())
  {
    ERROR("SpeedDialPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.speedDialPattern, speedDialPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("SpeedDialPattern failed to compile ('%s'): %s\n",
	  speedDialPattern.c_str(),
          strerror(errno));
    return -1;
  }


  m_patterns.reminderOnAnnouncement = aPath + "/" + cfg.getParameter("reminder_on_announcement", "");
  if(!file_exists(m_patterns.reminderOnAnnouncement)){
    ERROR("ReminderOnAnnouncement file does not exist ('%s').\n",
	  m_patterns.reminderOnAnnouncement.c_str());
    return -1;
  }
  reminderOnPattern = cfg.getParameter("reminder_on_pattern", "");
  if(reminderOnPattern.empty())
  {
    ERROR("ReminderOnPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.reminderOnPattern, reminderOnPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("ReminderOnPattern failed to compile ('%s'): %s\n",
	  reminderOnPattern.c_str(),
          strerror(errno));
    return -1;
  }
  
  
  m_patterns.reminderOffAnnouncement = aPath + "/" + cfg.getParameter("reminder_off_announcement", "");
  if(!file_exists(m_patterns.reminderOffAnnouncement)){
    ERROR("ReminderOffAnnouncement file does not exist ('%s').\n",
	  m_patterns.reminderOffAnnouncement.c_str());
    return -1;
  }
  reminderOffPattern = cfg.getParameter("reminder_off_pattern", "");
  if(reminderOffPattern.empty())
  {
    ERROR("ReminderOffPattern is empty\n");
    return -1;
  }
  if(regcomp(&m_patterns.reminderOffPattern, reminderOffPattern.c_str(), REG_EXTENDED|REG_NOSUB))
  {
    ERROR("ReminderOffPattern failed to compile ('%s'): %s\n",
	  reminderOffPattern.c_str(),
          strerror(errno));
    return -1;
  }

  return 0;
}

AmSession* SW_VscFactory::onInvite(const AmSipRequest& req)
{
  return new SW_VscDialog(&m_patterns, NULL);
}

AmSession* SW_VscFactory::onInvite(const AmSipRequest& req,
					 AmArg& session_params)
{
  UACAuthCred* cred = NULL;
  if (session_params.getType() == AmArg::AObject) {
    ArgObject* cred_obj = session_params.asObject();
    if (cred_obj)
      cred = dynamic_cast<UACAuthCred*>(cred_obj);
  }

  AmSession* s = new SW_VscDialog(&m_patterns, cred); 
  
  if (NULL == cred) {
    WARN("discarding unknown session parameters.\n");
  } else {
    AmSessionEventHandlerFactory* uac_auth_f = 
      AmPlugIn::instance()->getFactory4Seh("uac_auth");
    if (uac_auth_f != NULL) {
      DBG("UAC Auth enabled for new announcement session.\n");
      AmSessionEventHandler* h = uac_auth_f->getHandler(s);
      if (h != NULL )
	s->addHandler(h);
    } else {
      ERROR("uac_auth interface not accessible. "
	    "Load uac_auth for authenticated dialout.\n");
    }		
  }

  return s;
}

SW_VscDialog::SW_VscDialog(sw_vsc_patterns_t *patterns, UACAuthCred* credentials)
  : m_patterns(patterns), cred(credentials)
{
}

SW_VscDialog::~SW_VscDialog()
{
}

void SW_VscDialog::onSessionStart(const AmSipRequest& req)
{
  DBG("SW_VscDialog::onSessionStart\n");
  startSession(req);
}

void SW_VscDialog::onSessionStart(const AmSipReply& rep)
{
  DBG("SW_VscDialog::onSessionStart (SEMS originator mode)\n");
  startSession();
}

u_int64_t SW_VscDialog::getAttributeId(MYSQL *my_handler, const char* attribute)
{
  MYSQL_RES *res;
  MYSQL_ROW row;
  char query[1024] = "";
  u_int64_t id;

  snprintf(query, sizeof(query), SW_VSC_GET_ATTRIBUTE_ID, attribute);
	
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error fetching id for attribute '%s': %s", attribute, mysql_error(my_handler));
    return 0;
  }

  res = mysql_store_result(my_handler);
  if(mysql_num_rows(res) != 1)
  {
    ERROR("Found invalid number of id entries for attribute '%s': %llu", attribute, mysql_num_rows(res));
    return 0;
  }

  row = mysql_fetch_row(res);
  if(row == NULL || row[0] == NULL)
  {
    ERROR("Failed to fetch row for attribute id: %s\n", mysql_error(my_handler));
    return 0;
  }

  id = atoll(row[0]);
  mysql_free_result(res);
  return id;
}

u_int64_t SW_VscDialog::getSubscriberId(MYSQL *my_handler, const char* uuid, 
		string *domain, u_int64_t &domain_id)
{
  MYSQL_RES *res;
  MYSQL_ROW row;
  char query[1024] = "";
  u_int64_t id;

  snprintf(query, sizeof(query), SW_VSC_GET_SUBSCRIBER_ID, uuid);
	
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error fetching id for subscriber '%s': %s", uuid, mysql_error(my_handler));
    return 0;
  }

  res = mysql_store_result(my_handler);
  if(mysql_num_rows(res) != 1)
  {
    ERROR("Found invalid number of id entries for uuid '%s': %llu", uuid , mysql_num_rows(res));
    return 0;
  }

  row = mysql_fetch_row(res);
  if(row == NULL || row[0] == NULL || row[1] == NULL || row[2] == NULL)
  {
    ERROR("Failed to fetch row for uuid id: %s\n", mysql_error(my_handler));
    return 0;
  }

  id = atoll(row[0]);
  domain->clear();
  *domain += string(row[1]);
  domain_id = atoll(row[2]);
  mysql_free_result(res);
  return id;
}

u_int64_t SW_VscDialog::getPreference(MYSQL *my_handler, u_int64_t subscriberId, u_int64_t attributeId, 
  int *foundPref, string *value)
{
  MYSQL_RES *res;
  MYSQL_ROW row;
  char query[1024] = "";
  u_int64_t id;
  *foundPref = 0;

  snprintf(query, sizeof(query), SW_VSC_GET_PREFERENCE_ID, subscriberId, attributeId);
	
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error fetching preference id for subscriber id '%llu' and attribute id '%llu': %s", 
      subscriberId, attributeId, mysql_error(my_handler));
    return 0;
  }

  res = mysql_store_result(my_handler);
  if(mysql_num_rows(res) == 0)
  {
    mysql_free_result(res);
    return 1;
  }
  if(mysql_num_rows(res) != 1)
  {
    ERROR("Found invalid number of id entries for subscriber id '%llu' and attribute id '%llu': %llu", 
      subscriberId, attributeId, mysql_num_rows(res));
    mysql_free_result(res);
    return 0;
  }

  row = mysql_fetch_row(res);
  if(row == NULL || row[0] == NULL)
  {
    ERROR("Failed to fetch row for preference id: %s\n", mysql_error(my_handler));
    mysql_free_result(res);
    return 0;
  }
  id = atoll(row[0]);
  value->clear();
  *value += row[1];
  mysql_free_result(res);
  *foundPref = 1;
  return id;
}

int SW_VscDialog::deletePreferenceId(MYSQL *my_handler, u_int64_t preferenceId)
{
  char query[1024] = "";

  snprintf(query, sizeof(query), SW_VSC_DELETE_PREFERENCE_ID, preferenceId);
	
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error deleting preference id '%llu': %s", 
      preferenceId, mysql_error(my_handler));
    return 0;
  }
  return 1;
}

int SW_VscDialog::insertPreference(MYSQL *my_handler, u_int64_t subscriberId, 
	u_int64_t attributeId, string &uri)
{
  char query[1024] = "";

  snprintf(query, sizeof(query), SW_VSC_INSERT_PREFERENCE, subscriberId, attributeId, uri.c_str());
	
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error inserting preference for subscriber id '%llu': %s", 
      subscriberId, mysql_error(my_handler));
    return 0;
  }
  return 1;
}

int SW_VscDialog::updatePreferenceId(MYSQL *my_handler, u_int64_t preferenceId, string &uri)
{
  char query[1024] = "";

  snprintf(query, sizeof(query), SW_VSC_UPDATE_PREFERENCE_ID, uri.c_str(), preferenceId);
	
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error updating preference id '%llu': %s", 
      preferenceId, mysql_error(my_handler));
    return 0;
  }
  return 1;
}

int SW_VscDialog::insertReminder(MYSQL *my_handler, u_int64_t subscriberId, string &repeat, string &tim)
{
  char query[1024] = "";

  snprintf(query, sizeof(query), SW_VSC_INSERT_REMINDER, subscriberId, 
		  tim.c_str(), repeat.c_str());
	
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error setting reminder for subscriber id '%llu': %s", 
      subscriberId, mysql_error(my_handler));
    return 0;
  }
  return 1;
}

int SW_VscDialog::deleteReminder(MYSQL *my_handler, u_int64_t subscriberId)
{
  char query[1024] = "";

  snprintf(query, sizeof(query), SW_VSC_DELETE_REMINDER, subscriberId);
	
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error deleting reminder for subscriber id '%llu': %s", 
      subscriberId, mysql_error(my_handler));
    return 0;
  }
  return 1;
}

int SW_VscDialog::insertSpeedDialSlot(MYSQL *my_handler, u_int64_t subscriberId, string &slot, string &uri)
{
  char query[1024] = "";

  snprintf(query, sizeof(query), SW_VSC_INSERT_SPEEDDIAL, subscriberId, slot.c_str(), uri.c_str());
	
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error inserting speed-dial slot '%s' for subscriber id '%llu': %s", 
      slot.c_str(), subscriberId, mysql_error(my_handler));
    return 0;
  }
  return 1;
}


int SW_VscDialog::number2uri(const AmSipRequest& req, MYSQL *my_handler, string &uuid, u_int64_t subId,
		string &domain, u_int64_t domId, int offset, string &uri)
{
  string num = "", acStr = "", ccStr = "";
  u_int64_t acAttId, ccAttId;
  int foundPref;
  char query[1024] = "";
  my_ulonglong num_rows;
  MYSQL_RES *res;
  MYSQL_ROW row;
  RE *re;

  if(req.user.compare(0, 1, "*", 1) == 0)
  {
    num = req.user.substr(offset);

    if(m_patterns->voicemailNumber == num)
    {
      snprintf(query, sizeof(query), SW_VSC_GET_VOICEMAIL_NUMBER, uuid.c_str());
      if(mysql_real_query(my_handler, query, strlen(query)) != 0)
      {
        ERROR("Error fetching voicemail number for subscriber uuid '%s': %s", 
          uuid.c_str(), mysql_error(my_handler));
        return 0;
      }
      res = mysql_store_result(my_handler);
      row = mysql_fetch_row(res);
      if(row == NULL || row[0] == NULL)
      {
        ERROR("Failed to fetch row for voicemail number of uuid %s: %s\n", uuid.c_str(), 
          mysql_error(my_handler));
        mysql_free_result(res);
        return 0;
      }
      uri = string("sip:vmu") + row[0] + "@voicebox.local";
      INFO("Normalized '%s' to voicemail uri '%s' for uuid '%s'", num.c_str(), uri.c_str(), uuid.c_str());
      mysql_free_result(res);
      return 1;
    }

    snprintf(query, sizeof(query), SW_VSC_GET_USER_CALLEE_REWRITE_DPID, subId);
    if(mysql_real_query(my_handler, query, strlen(query)) != 0)
    {
      ERROR("Error fetching rewrite rule dpid for subscriber uuid '%s' (id %llu): %s", 
        uuid.c_str(), subId, mysql_error(my_handler));
      return 0;
    }
    res = mysql_store_result(my_handler);
    if((num_rows = mysql_num_rows(res)) != 0)
    {
      row = mysql_fetch_row(res);
      if(row == NULL || row[0] == NULL)
      {
        ERROR("Failed to fetch row for user callee rewrite rule: %s\n", mysql_error(my_handler));
        mysql_free_result(res);
        return 0;
      }
      u_int64_t dpid = atoll(row[0]);
      INFO("Found user rewrite rule dpid '%llu' for subscriber uuid '%s' (id %llu)", dpid, uuid.c_str(), subId);
      mysql_free_result(res);
      snprintf(query, sizeof(query), SW_VSC_GET_USER_CALLEE_REWRITES, dpid);
      if(mysql_real_query(my_handler, query, strlen(query)) != 0)
      {
        ERROR("Error fetching user callee rewrite rules for dpid '%llu' for subscriber uuid '%s': %s", 
          dpid, uuid.c_str(), mysql_error(my_handler));
        return 0;
      }
      res = mysql_store_result(my_handler);
      if((num_rows = mysql_num_rows(res)) == 0)
      {
        mysql_free_result(res);
        uri = string("sip:+") + num + "@" + domain;
        INFO("No user callee rewrite rules for subscriber uuid '%s' (id '%llu')",
          uuid.c_str(), subId);
        return 1;
      }
    }
    else
    {
      mysql_free_result(res);
      INFO("No user rewrite rule for subscriber uuid '%s' (id %llu), falling back to domain '%s' (id %llu)", 
	uuid.c_str(), subId, domain.c_str(), domId);
  
      snprintf(query, sizeof(query), SW_VSC_GET_DOMAIN_CALLEE_REWRITES, domId);
      if(mysql_real_query(my_handler, query, strlen(query)) != 0)
      {
        ERROR("Error fetching domain callee rewrite rules for domain id '%llu' (domain '%s') for subscriber uuid '%s': %s", 
          domId, domain.c_str(), uuid.c_str(), mysql_error(my_handler));
        return 0;
      }
      res = mysql_store_result(my_handler);
      if((num_rows = mysql_num_rows(res)) == 0)
      {
        mysql_free_result(res);
        uri = string("sip:+") + num + "@" + domain;
        INFO("No domain callee rewrite rules for domain id '%llu' (domain '%s') for subscriber uuid '%s'",
          domId, domain.c_str(), uuid.c_str());
        return 1;
      }
    }

    for(my_ulonglong i = 0; i < num_rows; ++i)
    {
      row = mysql_fetch_row(res);
      if(row == NULL || row[0] == NULL || row[1] == NULL)
      {
        ERROR("Failed to fetch row for callee rewrite rule: %s\n", mysql_error(my_handler));
        mysql_free_result(res);
        return 0;
      }

      re = new RE(row[0]);
      if(re == NULL || re->error().length() > 0)
      {
        ERROR("A callee rewrite rule match pattern ('%s') failed to compile: %s\n",
          row[0], (re ? re->error().c_str() : "unknown"));
        if(re) delete re;
        mysql_free_result(res);
        return 0;
      }
      if(re->Replace(row[1], &num))
      {
        INFO("The callee rewrite rule pattern ('%s' and '%s') for subscriber id %llu and domain id %llu matched, result is '%s'\n",
          row[0], row[1], subId, domId, num.c_str());
        delete re;
        break;
      }
      delete re;
    }

    mysql_free_result(res);

    // we need to replace $avp(s:caller_ac) and $avp(s:caller_cc) with the actual values here

    acAttId = getAttributeId(my_handler, "ac");
    if(!acAttId)
      return 0;
    ccAttId = getAttributeId(my_handler, "cc");
    if(!ccAttId)
      return 0;
    if(!getPreference(my_handler, subId, acAttId, &foundPref, &acStr))
      return 0;
    if(!getPreference(my_handler, subId, ccAttId, &foundPref, &ccStr))
      return 0;
    
    re = new RE("\\$avp\\(s:caller_ac\\)");
    if(re->GlobalReplace(acStr, &num))
    {
        INFO("Successfully replaced $avp(s:caller_ac) by preference value '%s' for subscriber id %llu and domain id %llu",
		acStr.c_str(), subId, domId);
    }
    delete re;
    re = new RE("\\$avp\\(s:caller_cc\\)");
    if(re->GlobalReplace(ccStr, &num))
    {
        INFO("Successfully replaced $avp(s:caller_cc) by preference value '%s' for subscriber id %llu and domain id %llu",
		ccStr.c_str(), subId, domId);
    }
    delete re;

    INFO("Final normalized number is '%s' for subscriber id %llu and domain id %llu",
	num.c_str(), subId, domId);
    uri = string("sip:+") + num + "@" + domain;
  }
  return 1;
}

u_int64_t SW_VscDialog::createCFMap(MYSQL *my_handler, u_int64_t subscriberId, string &uri, 
	const char* mapName, const char* type){
  u_int64_t dsetId;
  u_int64_t mapId;
  char query[1024] = "";

  // first, delete potentially existing vsc destination set for subscriber
  snprintf(query, sizeof(query), SW_VSC_DELETE_VSC_DESTSET, subscriberId, mapName);
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error deleting existing CF destination set '%s' for subscriber id '%llu': %s", 
          mapName, subscriberId, mysql_error(my_handler));
    return 0;
  }

  // create a new destination set
  snprintf(query, sizeof(query), SW_VSC_CREATE_VSC_DESTSET, subscriberId, mapName);
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error creating new CF destination set '%s' for subscriber id '%llu': %s", 
          mapName, subscriberId, mysql_error(my_handler));
    return 0;
  }
  dsetId = mysql_insert_id(my_handler);
  if(!dsetId)
  {
    ERROR("Error fetching last insert id of CF destination set for subscriber id '%llu'",
          subscriberId);
    return 0;
  }

  // create new destination entry
  snprintf(query, sizeof(query), SW_VSC_CREATE_VSC_DEST, dsetId, uri.c_str(), 1);
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error creating new CF destination '%s' for destination set id '%llu': %s", 
          uri.c_str(), dsetId, mysql_error(my_handler));
    return 0;
  }

  // delete existing mapping
  snprintf(query, sizeof(query), SW_VSC_DELETE_VSC_CFMAP, subscriberId, type);
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error deleting existing CF destination mapping for subscriber id '%llu' and type '%s': %s", 
          subscriberId, type, mysql_error(my_handler));
    return 0;
  }

  // create new mapping
  snprintf(query, sizeof(query), SW_VSC_CREATE_VSC_CFMAP, subscriberId, type, dsetId);
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error creating CF destination mapping for subscriber id '%llu' and type '%s' to destination set id '%llu': %s", 
          subscriberId, type, dsetId, mysql_error(my_handler));
    return 0;
  }
  mapId = mysql_insert_id(my_handler);
  if(!mapId)
  {
    ERROR("Error fetching last insert id of CF mapping for subscriber id '%llu' and type '%s'",
          subscriberId, type);
    return 0;
  }

  return mapId;

}

u_int64_t SW_VscDialog::deleteCFMap(MYSQL *my_handler, u_int64_t subscriberId, 
	const char* mapName, const char* type){
  char query[1024] = "";

  // first, delete potentially existing vsc destination set for subscriber
  snprintf(query, sizeof(query), SW_VSC_DELETE_VSC_DESTSET, subscriberId, mapName);
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error deleting existing CF destination set '%s' for subscriber id '%llu': %s", 
          mapName, subscriberId, mysql_error(my_handler));
    return 0;
  }

  // delete existing mapping
  snprintf(query, sizeof(query), SW_VSC_DELETE_VSC_CFMAP, subscriberId, type);
  if(mysql_real_query(my_handler, query, strlen(query)) != 0)
  {
    ERROR("Error deleting existing CF destination mapping for subscriber id '%llu' and type '%s': %s", 
          subscriberId, type, mysql_error(my_handler));
    return 0;
  }

  return 1;
}

void SW_VscDialog::startSession(const AmSipRequest& req){
 
  int ret;
  string filename;
  MYSQL *my_handler = NULL;
  my_bool recon = 1;

  u_int64_t subId, domId;
  string domain, uri;
  char map_str[128] = "";
  string mapStr, prefStr;
  int foundPref = 0;

  string uuid = getHeader(req.hdrs, "P-Caller-UUID");
  if(!uuid.length()) {
    ERROR("Application header P-Caller-UUID not found\n");
    throw AmSession::Exception(500, "could not get UUID parameter");
  }


  my_handler = mysql_init(NULL);
  if(!mysql_real_connect(my_handler, 
    m_patterns->mysqlHost.c_str(), m_patterns->mysqlUser.c_str(), m_patterns->mysqlPass.c_str(),
    SW_VSC_DATABASE, m_patterns->mysqlPort, NULL, 0))
  {
    ERROR("Error connecting to provisioning db: %s", mysql_error(my_handler));
    filename = m_patterns->failAnnouncement;
    goto out;
  }
  if(mysql_options(my_handler, MYSQL_OPT_RECONNECT, &recon) != 0)
  {
    ERROR("Error setting reconnect-option for provisioning db: %s", mysql_error(my_handler));
    filename = m_patterns->failAnnouncement;
    goto out;
  }

  subId = getSubscriberId(my_handler, uuid.c_str(), &domain, domId);
  if(!subId)
  {
    filename = m_patterns->failAnnouncement;
    goto out;
  }
  
  DBG("Trigger VSC for uuid '%s' (sid='%llu')\n", 
		  uuid.c_str(), subId);

  setReceiving(false);

  if((ret = regexec(&m_patterns->cfuOnPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
 	if(!number2uri(req, my_handler, uuid, subId, domain, domId, 4, uri))
	{
	  	filename = m_patterns->failAnnouncement;
		goto out;
	}

	u_int64_t mapId = createCFMap(my_handler, subId, uri, SW_VSC_DESTSET_CFU, "cfu");
	if(!mapId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	snprintf(map_str, sizeof(mapStr), "%llu", mapId);
	mapStr = string(map_str);

	u_int64_t attId = getAttributeId(my_handler, "cfu");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(!foundPref)
	{
		if(!insertPreference(my_handler, subId, attId, mapStr))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully set VSC CFU to '%s' using mapping id '%llu' for uuid '%s'",
			uri.c_str(), mapId, uuid.c_str());
	}
	else
	{
		if(!updatePreferenceId(my_handler, prefId, mapStr))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully updated VSC CFU to '%s' using mapping id '%llu' for uuid '%s'",
			uri.c_str(), mapId, uuid.c_str());
	}

	filename = m_patterns->cfuOnAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }
 
  if((ret = regexec(&m_patterns->cfuOffPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
	if(!deleteCFMap(my_handler, subId, SW_VSC_DESTSET_CFU, "cfu"))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t attId = getAttributeId(my_handler, "cfu");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(!foundPref)
	{
		INFO("Unnecessary VSC CFU removal for uuid '%s'", 
			uuid.c_str());
	}
	else if(!deletePreferenceId(my_handler, prefId))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else
	{
		INFO("Successfully removed VSC CFU for uuid '%s'", 
			uuid.c_str());
	}
  	
	filename = m_patterns->cfuOffAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }
 
  if((ret = regexec(&m_patterns->cfbOnPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
 	if(!number2uri(req, my_handler, uuid, subId, domain, domId, 4, uri))
	{
	  	filename = m_patterns->failAnnouncement;
		goto out;
	}

	u_int64_t mapId = createCFMap(my_handler, subId, uri, SW_VSC_DESTSET_CFB, "cfb");
	if(!mapId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	snprintf(map_str, sizeof(mapStr), "%llu", mapId);
	mapStr = string(map_str);

	u_int64_t attId = getAttributeId(my_handler, "cfb");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(!foundPref)
	{
		if(!insertPreference(my_handler, subId, attId, mapStr))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully set VSC CFB to '%s' for uuid '%s'",
			uri.c_str(), uuid.c_str());
	}
	else
	{
		if(!updatePreferenceId(my_handler, prefId, mapStr))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully updated VSC CFB to '%s' for uuid '%s'",
			uri.c_str(), uuid.c_str());
	}

	filename = m_patterns->cfbOnAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }
 
  if((ret = regexec(&m_patterns->cfbOffPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
	if(!deleteCFMap(my_handler, subId, SW_VSC_DESTSET_CFB, "cfb"))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t attId = getAttributeId(my_handler, "cfb");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(!foundPref)
	{
		INFO("Unnecessary VSC CFB removal for uuid '%s'", 
			uuid.c_str());
	}
	else if(!deletePreferenceId(my_handler, prefId))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else
	{
		INFO("Successfully removed VSC CFB for uuid '%s'", 
			uuid.c_str());
	}
  	
	filename = m_patterns->cfbOffAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }

  if((ret = regexec(&m_patterns->cftOnPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
	string::size_type timend = req.user.find('*', 4);
        string tim = req.user.substr(4, timend-4);
        INFO("Extracted ringtimeout of '%s' from '%s' for uuid '%s'", tim.c_str(), 
          req.user.c_str(), uuid.c_str());

 	if(!number2uri(req, my_handler, uuid, subId, domain, domId, timend+1, uri))
	{
	  	filename = m_patterns->failAnnouncement;
		goto out;
	}

	u_int64_t mapId = createCFMap(my_handler, subId, uri, SW_VSC_DESTSET_CFT, "cft");
	if(!mapId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	snprintf(map_str, sizeof(mapStr), "%llu", mapId);
	mapStr = string(map_str);

	u_int64_t attId = getAttributeId(my_handler, "cft");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(!foundPref)
	{
		if(!insertPreference(my_handler, subId, attId, mapStr))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully set VSC CFT to '%s' for uuid '%s'",
			uri.c_str(), uuid.c_str());
	}
	else
	{
		if(!updatePreferenceId(my_handler, prefId, mapStr))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully updated VSC CFT to '%s' for uuid '%s'",
			uri.c_str(), uuid.c_str());
	}

	attId = getAttributeId(my_handler, "ringtimeout");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(!foundPref)
	{
		if(!insertPreference(my_handler, subId, attId, tim))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully set VSC ringtimeout to '%s' for uuid '%s'",
			tim.c_str(), uuid.c_str());
	}
	else
	{
		if(!updatePreferenceId(my_handler, prefId, tim))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully updated VSC ringtimeout to '%s' for uuid '%s'",
			tim.c_str(), uuid.c_str());
	}

	filename = m_patterns->cftOnAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }
 
  if((ret = regexec(&m_patterns->cftOffPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
	if(!deleteCFMap(my_handler, subId, SW_VSC_DESTSET_CFT, "cft"))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t attId = getAttributeId(my_handler, "cft");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}

	u_int64_t prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(!foundPref)
	{
		INFO("Unnecessary VSC CFT removal for uuid '%s'", 
			uuid.c_str());
	}
	else if(!deletePreferenceId(my_handler, prefId))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else
	{
		INFO("Successfully removed VSC CFT for uuid '%s'", 
			uuid.c_str());
	}

	attId = getAttributeId(my_handler, "ringtimeout");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(foundPref && !deletePreferenceId(my_handler, prefId))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else
	{
		INFO("Successfully removed VSC CFT ringtimeout for uuid '%s'", 
			uuid.c_str());
	}

  	
	filename = m_patterns->cftOffAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }

  if((ret = regexec(&m_patterns->cfnaOnPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
 	if(!number2uri(req, my_handler, uuid, subId, domain, domId, 4, uri))
	{
	  	filename = m_patterns->failAnnouncement;
		goto out;
	}

	u_int64_t mapId = createCFMap(my_handler, subId, uri, SW_VSC_DESTSET_CFNA, "cfna");
	if(!mapId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	snprintf(map_str, sizeof(mapStr), "%llu", mapId);
	mapStr = string(map_str);

	u_int64_t attId = getAttributeId(my_handler, "cfna");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(!foundPref)
	{
		if(!insertPreference(my_handler, subId, attId, mapStr))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully set VSC CFNA to '%s' for uuid '%s'",
			uri.c_str(), uuid.c_str());
	}
	else
	{
		if(!updatePreferenceId(my_handler, prefId, mapStr))
		{
		  filename = m_patterns->failAnnouncement;
		  goto out;
		}
  		INFO("Successfully updated VSC CFNA to '%s' for uuid '%s'",
			uri.c_str(), uuid.c_str());
	}

	filename = m_patterns->cfnaOnAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }
 
  if((ret = regexec(&m_patterns->cfnaOffPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
	if(!deleteCFMap(my_handler, subId, SW_VSC_DESTSET_CFNA, "cfna"))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t attId = getAttributeId(my_handler, "cfna");
	if(!attId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	u_int64_t prefId = getPreference(my_handler, subId, attId, &foundPref, &prefStr);
	if(!prefId)
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else if(!foundPref)
	{
		INFO("Unnecessary VSC CFNA removal for uuid '%s'", 
			uuid.c_str());
	}
	else if(!deletePreferenceId(my_handler, prefId))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else
	{
		INFO("Successfully removed VSC CFNA for uuid '%s'", 
			uuid.c_str());
	}
  	
	filename = m_patterns->cfnaOffAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }

  if((ret = regexec(&m_patterns->speedDialPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
	string slot = string("*") + req.user.substr(4, 1);
 	if(!number2uri(req, my_handler, uuid, subId, domain, domId, 5, uri))
	{
	  	filename = m_patterns->failAnnouncement;
		goto out;
	}
	if(!insertSpeedDialSlot(my_handler, subId, slot, uri))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else
	{
		INFO("Successfully set speed-dial slot '%s' for uuid '%s'", 
			slot.c_str(), uuid.c_str());
	}
  	
	filename = m_patterns->speedDialAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }

  if((ret = regexec(&m_patterns->reminderOnPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
	int hour, min;
	string tim; char c_tim[6] = "";
	hour = atoi(req.user.substr(4, 2).c_str());
	min = atoi(req.user.substr(6, 2).c_str());

	if (hour < 0 || hour > 23)
	{
		filename = m_patterns->failAnnouncement;
		INFO("Invalid hour '%s' in reminder data for uuid '%s'",
			req.user.substr(4, 2).c_str(), uuid.c_str());
		goto out;
	}
	if (min < 0 || min > 59)
	{
		filename = m_patterns->failAnnouncement;
		INFO("Invalid minute '%s' in reminder data for uuid '%s'",
			req.user.substr(6, 2).c_str(), uuid.c_str());
		goto out;
	}
	snprintf(c_tim, sizeof(c_tim), "%02d:%02d", hour, min);
	tim = string(c_tim);
	string recur("never");
	
	if(!insertReminder(my_handler, subId, recur, tim))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else
	{
		INFO("Successfully set reminder at '%s' for uuid '%s'", 
			c_tim, uuid.c_str());
	}
  	
	filename = m_patterns->reminderOnAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }

  if((ret = regexec(&m_patterns->reminderOffPattern, req.user.c_str(), 0, 0, 0)) == 0)
  {
	if(!deleteReminder(my_handler, subId))
	{
		filename = m_patterns->failAnnouncement;
		goto out;
	}
	else
	{
		INFO("Successfully deleted reminder for uuid '%s'", 
			uuid.c_str());
	}
  	
	filename = m_patterns->reminderOffAnnouncement;
	goto out;
  }
  else if(ret != REG_NOMATCH)
  {
  	filename = m_patterns->failAnnouncement;
	goto out;
  }



  INFO("Unkown VSC code '%s' found", req.user.c_str());
  filename = m_patterns->unknownAnnouncement;
 
  out:

  if(my_handler != NULL)
  {
    mysql_close(my_handler);
  }
  if(m_wav_file.open(filename, AmAudioFile::Read))
    throw string("SW_VscDialog::onSessionStart: Cannot open file\n");

    
  setOutput(&m_wav_file);
}

void SW_VscDialog::startSession(){
  AmSipRequest dummy;
  startSession(dummy);
}

void SW_VscDialog::onBye(const AmSipRequest& req)
{
  DBG("onBye: stopSession\n");
  setStopped();
}


void SW_VscDialog::process(AmEvent* event)
{

  AmAudioEvent* audio_event = dynamic_cast<AmAudioEvent*>(event);
  if(audio_event && (audio_event->event_id == AmAudioEvent::cleared)){
    dlg.bye();
    setStopped();
    return;
  }

  AmSession::process(event);
}

inline UACAuthCred* SW_VscDialog::getCredentials() {
  return cred.get();
}