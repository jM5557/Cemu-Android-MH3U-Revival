#include "NetworkSettings.h"
#include "ActiveSettings.h"
#include "LaunchSettings.h"
#include "CemuConfig.h"
#include "Common/FileStream.h"

XMLNetworkConfig_t n_config(L"network_services.xml");


void NetworkConfig::LoadOnce() 
{
	n_config.SetFilename(ActiveSettings::GetConfigPath("network_services.xml").generic_wstring());
	if (XMLExists())
		n_config.Load();
}

void NetworkConfig::Load(XMLConfigParser& parser) 
{
	auto config = parser.get("content");
	networkname = config.get("networkname", "Custom");
	disablesslver = config.get("disablesslverification", disablesslver);
	auto u = config.get("urls");
	urls.ACT = u.get("act", NintendoURLs::ACTURL);
	urls.ECS = u.get("ecs", NintendoURLs::ECSURL);
	urls.NUS = u.get("nus", NintendoURLs::NUSURL);
	urls.IAS = u.get("ias", NintendoURLs::IASURL);
	urls.CCSU = u.get("ccsu", NintendoURLs::CCSUURL);
	urls.CCS = u.get("ccs", NintendoURLs::CCSURL);
	urls.IDBE = u.get("idbe", NintendoURLs::IDBEURL);
	urls.BOSS = u.get("boss", NintendoURLs::BOSSURL);
	urls.TAGAYA = u.get("tagaya", NintendoURLs::TAGAYAURL);
	urls.OLV = u.get("olv", NintendoURLs::OLVURL);
}

// File-scope rather than function-local so it can be invalidated. Anything that creates
// network_services.xml at runtime (the Android dumpless-online setup) must call
// InvalidateXMLExistsCache(), otherwise the "false" cached during startup sticks for the whole
// session and selecting the Custom service silently reads back as Offline.
static std::optional<bool> s_networkXmlExists;

void NetworkConfig::InvalidateXMLExistsCache()
{
	s_networkXmlExists.reset();
}

bool NetworkConfig::XMLExists() 
{
	auto& s_exists = s_networkXmlExists;
	if(s_exists.has_value())
		return *s_exists;
	std::error_code ec;
	if (!fs::exists(ActiveSettings::GetConfigPath("network_services.xml"), ec))
	{
		s_exists = false;
		return false;
	}
	s_exists = true;
	return true;
}