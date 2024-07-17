/*	EQEMu: Everquest Server Emulator
	Copyright (C) 2001-2010 EQEMu Development Team (http://eqemulator.net)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE. See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/
#include "world_server.h"
#include "login_server.h"
#include "login_types.h"
#include "../common/eqemu_logsys.h"
#include "../common/ip_util.h"

extern EQEmuLogSys LogSys;
extern LoginServer server;

WorldServer::WorldServer(EmuTCPConnection *c)
{
	m_connection = c;
	m_zones_booted = 0;
	m_players_online = 0;
	m_server_status = 0;
	m_runtime_id = 0;
	m_server_list_type_id = 0;
	m_server_process_type = 0;
	m_is_server_authorized = false;
	m_is_server_trusted = false;
	m_is_server_logged_in = false;
}

WorldServer::~WorldServer()
{
	if(m_connection) {
		m_connection->Free();
	}
}

void WorldServer::Reset()
{
	m_zones_booted = 0;
	m_players_online = 0;
	m_server_status = 0;
	m_runtime_id;
	m_server_list_type_id = 0;
	m_server_process_type = 0;
	m_is_server_authorized = false;
	m_is_server_logged_in = false;
}

bool WorldServer::Process()
{
	ServerPacket *app = nullptr;
	while(app = m_connection->PopPacket()) {
		LogNetcode(
			"[ProcessNewLSInfo] Application packet received from server [Size: {}]\n{}",
			app->size, 
			DumpServerPacketToString(app)
		);

		switch(app->opcode)
		{
		case ServerOP_NewLSInfo:
			{
				if(app->size < sizeof(ServerNewLSInfo_Struct))
				{
					LogError("Received application packet from server that had opcode ServerOP_NewLSInfo, "
						"but was too small. Discarded to avoid buffer overrun.");
					break;
				}

				LogInfo("New Login Info Recieved.");
				ServerNewLSInfo_Struct *info = (ServerNewLSInfo_Struct*)app->pBuffer;
				Handle_NewLSInfo(info);
				break;
			}
		case ServerOP_LSStatus:
			{
				if(app->size < sizeof(ServerLSStatus_Struct))
				{
					LogError("Recieved application packet from server that had opcode ServerOP_LSStatus, "
						"but was too small. Discarded to avoid buffer overrun.");
					break;
				}

				LogDebug("World Server Status Recieved.");

				auto *ls_status = (ServerLSStatus_Struct*)app->pBuffer;

				LogDebug("World Server Status Update | Server [{0}] Status [{1}] Players [{2}] Zones [{3}]",
					GetServerLongName(), ls_status->status, ls_status->num_players, ls_status->num_zones);

				Handle_LSStatus(ls_status);
				break;
			}
		case ServerOP_LSZoneInfo:
		case ServerOP_LSZoneShutdown:
		case ServerOP_LSZoneStart:
		case ServerOP_LSZoneBoot:
		case ServerOP_LSZoneSleep:
		case ServerOP_LSPlayerLeftWorld:
		case ServerOP_LSPlayerJoinWorld:
		case ServerOP_LSPlayerZoneChange:
			{
				//Not logging these to cut down on spam until we implement them
				break;
			}

		case ServerOP_UsertoWorldResp:
			{
				if(app->size < sizeof(UsertoWorldResponse_Struct))
				{
					LogError("Recieved application packet from server that had opcode ServerOP_UsertoWorldResp, "
						"but was too small. Discarded to avoid buffer overrun.");
					break;
				}

				LogInfo("User-To-World Response received.");

				auto *user_to_world_response = (UsertoWorldResponse_Struct*)app->pBuffer;
				LogInfo("Trying to find client with user id of [{0}].", user_to_world_response->lsaccountid);
				Client *c = server.client_manager->GetClient(user_to_world_response->lsaccountid);
				if (c && c->GetClientVersion() == cv_old)
				{
					if (user_to_world_response->response > 0)
					{
						SendClientAuth(c->GetConnection()->GetRemoteIP(), c->GetAccountName(), c->GetKey(), c->GetAccountID(), c->GetMacClientVersion());
					}

					switch (user_to_world_response->response)
					{
					case UserToWorldStatusSuccess:
						break;
					case UserToWorldStatusWorldUnavail:
						c->FatalError("\nError 1020: Your chosen World Server is DOWN.\n\nPlease select another.");
						break;
					case UserToWorldStatusSuspended:
						c->FatalError("You have been suspended from the worldserver.");
						break;
					case UserToWorldStatusBanned:
						c->FatalError("You have been banned from the worldserver.");
						break;
					case UserToWorldStatusWorldAtCapacity:
						c->FatalError("That server is full.");
						break;
					case UserToWorldStatusAlreadyOnline:
						c->FatalError("Error 1018: You currently have an active character on that EverQuest Server, please allow a minute for synchronization and try again.");
						break;
					case UserToWorldStatusIPLimitExceeded:
						c->FatalError("Error IP Limit Exceeded: \n\nYou have exceeded the maximum number of allowed IP addresses for this account.");
						break;
					}
					LogInfo("Found client with user id of {0} and account name of {1}.", user_to_world_response->lsaccountid, c->GetAccountName().c_str());
					EQApplicationPacket* outapp = new EQApplicationPacket(OP_PlayEverquestRequest, 17);
					strncpy((char*)&outapp->pBuffer[1], c->GetKey().c_str(), c->GetKey().size());

					c->SendPlayResponse(outapp);
					delete outapp;
				}
				else if (c)
				{
					LogInfo("Found client with user id of [{0}] and account name of [{1}].", user_to_world_response->lsaccountid, c->GetAccountName().c_str());
					auto outapp = new EQApplicationPacket(OP_PlayEverquestResponse, sizeof(PlayEverquestResponse_Struct));
					PlayEverquestResponse_Struct *per = (PlayEverquestResponse_Struct*)outapp->pBuffer;
					per->Sequence = c->GetPlaySequence();
					per->ServerNumber = c->GetPlayServerID();
					LogInfo("Found sequence and play of [{0}] [{1}]", c->GetPlaySequence(), c->GetPlayServerID());
					LogDebug("[Size: {0}] [{1}]", outapp->size, DumpPacketToString(outapp).c_str());

					if(user_to_world_response->response > 0)
					{
						per->Allowed = 1;
						SendClientAuth(c->GetConnection()->GetRemoteIP(), c->GetAccountName(), c->GetKey(), c->GetAccountID());
					}

					switch(user_to_world_response->response)
					{
					case UserToWorldStatusSuccess:
						per->Message = LS::ErrStr::NON_ERROR;
						break;
					case UserToWorldStatusWorldUnavail:
						per->Message = LS::ErrStr::SERVER_UNAVAILABLE;
						break;
					case UserToWorldStatusSuspended:
						per->Message = LS::ErrStr::ACCOUNT_SUSPENDED;
						break;
					case UserToWorldStatusBanned:
						per->Message = LS::ErrStr::ACCOUNT_BANNED;
						break;
					case UserToWorldStatusWorldAtCapacity:
						per->Message = LS::ErrStr::WORLD_MAX_CAPACITY;
						break;
					case UserToWorldStatusAlreadyOnline:
						per->Message = LS::ErrStr::ERROR_1018_ACTIVE_CHARACTER;
						break;
					case UserToWorldStatusIPLimitExceeded:
						per->Message = LS::ErrStr::IP_ADDR_MAX;
						break;
					}

					LogInfo("Sending play response with following data, allowed {} , sequence {} , server number {} , message {} ",
							per->Allowed, per->Sequence, per->ServerNumber, per->Message);

					c->SendPlayResponse(outapp);
					delete outapp;
				}
				else
				{
					LogError("Received User-To-World Response for [{0}] but could not find the client referenced!.", user_to_world_response->lsaccountid);
				}
				break;
			}
		case ServerOP_LSAccountUpdate:
			{
				if(app->size < sizeof(ServerLSAccountUpdate_Struct))
				{
					LogError("Recieved application packet from server that had opcode ServerLSAccountUpdate_Struct, "
						"but was too small. Discarded to avoid buffer overrun.");
					break;
				}
			
				LogInfo("ServerOP_LSAccountUpdate packet received from: {0}", m_short_name.c_str());
				ServerLSAccountUpdate_Struct *lsau = (ServerLSAccountUpdate_Struct*)app->pBuffer;
				if(m_is_server_trusted)
				{
					LogInfo("ServerOP_LSAccountUpdate update processed for: {0}", lsau->useraccount);
					string name;
					string password;
					string email;
					name.assign(lsau->useraccount);
					password.assign(lsau->userpassword);
					email.assign(lsau->useremail);
					server.db->UpdateLSAccountInfo(lsau->useraccountid, name, password, email);
				}
				break;
			}
		default:
			{
				LogError("Recieved application packet from server that had an unknown operation code 0x%.4X.", app->opcode);
			}
		}

		delete app;
		app = nullptr;
	}
	return true;
}

void WorldServer::Handle_NewLSInfo(ServerNewLSInfo_Struct* i)
{
	if(m_is_server_logged_in)
	{
		LogError("WorldServer::Handle_NewLSInfo called but the login server was already marked as logged in, aborting.");
		return;
	}

	if(strlen(i->account) <= 30)
	{
		m_account_name = i->account;
	}
	else
	{
		LogError("Handle_NewLSInfo error, account name was too long.");
		return;
	}

	if(strlen(i->password) <= 30)
	{
		m_account_password = i->password;
	}
	else
	{
		LogError("Handle_NewLSInfo error, account password was too long.");
		return;
	}

	if(strlen(i->name) <= 200)
	{
		m_long_name = i->name;
	}
	else
	{
		LogError("Handle_NewLSInfo error, long name was too long.");
		return;
	}

	if(strlen(i->shortname) <= 50)
	{
		m_short_name = i->shortname;
	}
	else
	{
		LogError("Handle_NewLSInfo error, short name was too long.");
		return;
	}

	if(strlen(i->local_address) <= 125)
	{
		if(strlen(i->local_address) == 0)
		{
			LogError("Handle_NewLSInfo error, local address was null, defaulting to localhost");
			m_local_ip = "127.0.0.1";
		}
		else
		{
			m_local_ip = i->local_address;
		}
	}
	else
	{
		LogError("Handle_NewLSInfo error, local address was too long.");
		return;
	}

	if(strlen(i->remote_address) <= 125)
	{
		if(strlen(i->remote_address) == 0)
		{
			in_addr in;
			in.s_addr = GetConnection()->GetrIP();
			m_remote_ip_address = inet_ntoa(in);
			LogError("Handle_NewLSInfo error, remote address was null, defaulting to stream address %s.", m_remote_ip_address.c_str());
		}
		else
		{
			m_remote_ip_address = i->remote_address;
		}
	}
	else
	{
		in_addr in;
		in.s_addr = GetConnection()->GetrIP();
		m_remote_ip_address = inet_ntoa(in);
		LogError("Handle_NewLSInfo error, remote address was too long, defaulting to stream address %s.", m_remote_ip_address.c_str());
	}

	if(strlen(i->serverversion) <= 64)
	{
		m_version = i->serverversion;
	}
	else
	{
		LogError("Handle_NewLSInfo error, server version was too long.");
		return;
	}

	if(strlen(i->protocolversion) <= 25)
	{
		m_protocol = i->protocolversion;
	}
	else
	{
		LogError("Handle_NewLSInfo error, protocol version was too long.");
		return;
	}

	m_server_process_type = i->servertype;
	m_is_server_logged_in = true;

	if(server.options.IsRejectingDuplicateServers())
	{
		if(server.server_manager->ServerExists(m_long_name, m_short_name, this))
		{
			LogError("World tried to login but there already exists a server that has that name.");
			return;
		}
	}
	else
	{
		if(server.server_manager->ServerExists(m_long_name, m_short_name, this))
		{
			LogError("World tried to login but there already exists a server that has that name.");
			server.server_manager->DestroyServerByName(m_long_name, m_short_name, this);
		}
	}

	if(!server.options.IsUnregisteredAllowed())
	{
		if(m_account_name.size() > 0 && m_account_password.size() > 0)
		{
			unsigned int s_id = 0;
			unsigned int s_list_type = 0;
			unsigned int s_trusted = 0;
			string s_desc;
			string s_list_desc;
			string s_acct_name;
			string s_acct_pass;
			if(server.db->GetWorldRegistration(m_long_name, m_short_name, s_id, s_desc, s_list_type, s_trusted, s_list_desc, s_acct_name, s_acct_pass))
			{
				if(s_acct_name.size() == 0 || s_acct_pass.size() == 0)
				{
					LogInfo("Server [{0}]([{1}]) successfully logged into account that had no user/password requirement.",
						m_long_name.c_str(), m_short_name.c_str());
					m_is_server_authorized = true;
					SetRuntimeID(s_id);
					m_server_list_type_id = s_list_type;
					m_server_description = s_desc;
				}
				else if(s_acct_name.compare(m_account_name) == 0 && s_acct_pass.compare(m_account_password) == 0)
				{
					LogInfo("Server [{0}]({1}) successfully logged in.",
						m_long_name.c_str(), m_short_name.c_str());
					m_is_server_authorized = true;
					SetRuntimeID(s_id);
					m_server_list_type_id = s_list_type;
					m_server_description = s_desc;
					if(s_trusted) {
						LogInfo("ServerOP_LSAccountUpdate sent to world");
						m_is_server_trusted = true;
						auto outapp = new ServerPacket(ServerOP_LSAccountUpdate, 0);
						m_connection->SendPacket(outapp);
						safe_delete(outapp);
					}
				}
				else {
					LogInfo("Server [{0}]([{1}]) attempted to log in but account and password did not match the entry in the database, and only"
						" registered servers are allowed.", m_long_name.c_str(), m_short_name.c_str());
					return;
				}
			}
			else {
				LogInfo("Server [{0}]([{1}]) attempted to log in but database couldn't find an entry and only registered servers are allowed.",
					m_long_name.c_str(), m_short_name.c_str());
				return;
			}
		}
		else {
			LogInfo("Server [{0}]([{1}]) did not attempt to log in but only registered servers are allowed.",
				m_long_name.c_str(), m_short_name.c_str());
			return;
		}
	}
	else {
		unsigned int server_id = 0;
		unsigned int server_list_type = 0;
		unsigned int is_server_trusted = 0;
		string server_description;
		string server_list_description;
		string server_account_name;
		string server_account_password;


		if(server.db->GetWorldRegistration(
			m_long_name,
			m_short_name,
			server_id, 
			server_description, 
			server_list_type, 
			is_server_trusted, 
			server_list_description, 
			server_account_name, 
			server_account_password)) 
		{
			
			if(m_account_name.size() > 0 && m_account_password.size() > 0) {
				if(server_account_name.compare(m_account_name) == 0 && server_account_password.compare(m_account_password) == 0) {
					LogInfo("Server [{0}]([{1}]) successfully logged in.", m_long_name.c_str(), m_short_name.c_str());
					m_is_server_authorized = true;
					SetRuntimeID(server_id);
					m_server_list_type_id = server_list_type;
					m_server_description = server_description;

					if(is_server_trusted) {
						LogInfo("ServerOP_LSAccountUpdate sent to world");
						is_server_trusted = true;
						auto outapp = new ServerPacket(ServerOP_LSAccountUpdate, 0);
						m_connection->SendPacket(outapp);
						safe_delete(outapp);
					}
				}
				else {
					// this is the first of two cases where we should deny access even if unregistered is allowed
					LogInfo("Server [{0}]([{1}]) attempted to log in but account and password did not match the entry in the database.",
						m_long_name.c_str(), m_short_name.c_str());
				}
			}
			else {
				if(server_account_name.size() > 0 || server_account_password.size() > 0) {
					// this is the second of two cases where we should deny access even if unregistered is allowed
					LogInfo("Server  [{0}]([{1}]) did not attempt to log in but this server requires a password.",
						m_long_name.c_str(), m_short_name.c_str());
				}
				else {
					LogInfo("Server  [{0}]([{1}]) did not attempt to log in but unregistered servers are allowed.",
						m_long_name.c_str(), m_short_name.c_str());
					m_is_server_authorized = true;
					SetRuntimeID(server_id);
					m_server_list_type_id = 0;
				}
			}
		}
		else
		{
			LogInfo("Server [{0}]([{1}]) attempted to log in but database couldn't find an entry but unregistered servers are allowed.",
				m_long_name.c_str(), m_short_name.c_str());
			if(server.db->CreateWorldRegistration(m_long_name, m_short_name, server_id)) {
				m_is_server_authorized = true;
				SetRuntimeID(server_id);
				m_server_list_type_id = 0;
			}
		}
	}

	in_addr in;
	in.s_addr = m_connection->GetrIP();
	server.db->UpdateWorldRegistration(GetRuntimeID(), m_long_name, string(inet_ntoa(in)));

	if (m_is_server_authorized)
	{
		server.client_manager->UpdateServerList();
	}
}

void WorldServer::Handle_LSStatus(ServerLSStatus_Struct *s)
{
	m_players_online = s->num_players;
	m_zones_booted = s->num_zones;
	m_server_status = s->status;
}

void WorldServer::SendClientAuth(unsigned int ip, string account, string key, unsigned int account_id, uint8 version)
{
	auto outapp = new ServerPacket(ServerOP_LSClientAuth, sizeof(ServerLSClientAuth));
	ServerLSClientAuth* client_auth = (ServerLSClientAuth*)outapp->pBuffer;

	client_auth->lsaccount_id = account_id;
	strncpy(client_auth->name, account.c_str(), account.size() > 30 ? 30 : account.size());
	strncpy(client_auth->key, key.c_str(), 10);
	client_auth->lsadmin = 0;
	client_auth->worldadmin = 0;
	client_auth->ip = ip;
	client_auth->version = version;

	in_addr in{};
	in.s_addr = ip; m_connection->GetrIP();
	string client_address(inet_ntoa(in));
	in.s_addr = m_connection->GetrIP();
	string world_address(inet_ntoa(in));

	if(client_address.compare(world_address) == 0) {
		client_auth->local = 1;
	}
	else if (IpUtil::IsIpInPrivateRfc1918(client_address)) {
		LogInfo("Client is authenticating from a local address [{0}]", client_address);
		client_auth->local = 1;
	}
	else {
		client_auth->local = 0;
	}

	m_connection->SendPacket(outapp);

	LogNetcode(
		"[Size: [{}] [{}]", 
		outapp->size, 
		DumpServerPacketToString(outapp)
	);

	safe_delete(outapp);
}

