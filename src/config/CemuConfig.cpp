#include "config/CemuConfig.h"
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "Cafe/Account/Account.h"

void CemuConfig::SetMLCPath(fs::path path)
{
	mlc_path.SetValue(_pathToUtf8(path));
	Account::RefreshAccounts();
}

NetworkService CemuConfig::GetAccountNetworkService(uint32 persistentId)
{
	auto it = account.service_select.find(persistentId);
	if (it != account.service_select.end())
	{
		NetworkService serviceIndex = it->second;
		// make sure the returned service is valid
		if (serviceIndex != NetworkService::Offline &&
			serviceIndex != NetworkService::Nintendo &&
			serviceIndex != NetworkService::Pretendo &&
			serviceIndex != NetworkService::Custom)
			return NetworkService::Offline;
		if( static_cast<NetworkService>(serviceIndex) == NetworkService::Custom && !NetworkConfig::XMLExists() )
			return NetworkService::Offline; // custom is selected but no custom config exists
		return serviceIndex;
	}
	// if not found, return the legacy value
	if(!account.legacy_online_enabled)
		return NetworkService::Offline;
	return static_cast<NetworkService>(account.legacy_active_service.GetValue() + 1); // +1 because "Offline" now takes index 0
}

void CemuConfig::SetAccountSelectedService(uint32 persistentId, NetworkService serviceIndex)
{
	account.service_select[persistentId] = serviceIndex;
}
