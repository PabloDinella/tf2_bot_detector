#ifdef TF2BD_ENABLE_DISCORD_INTEGRATION
#include "DiscordRichPresence.h"
#include "Config/DRPInfo.h"
#include "Config/Settings.h"
#include "ConsoleLog/ConsoleLineListener.h"
#include "ConsoleLog/ConsoleLines.h"
#include "ConsoleLog/NetworkStatus.h"
#include "GameData/MatchmakingQueue.h"
#include "GameData/TFClassType.h"
#include "Log.h"
#include "WorldEventListener.h"
#include "WorldState.h"

#include <mh/text/charconv_helper.hpp>
#include <mh/text/format.hpp>
#include <mh/text/string_insertion.hpp>
#include <discord-game-sdk/core.h>

#include <array>
#include <cassert>
#include <compare>

using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace tf2_bot_detector;

namespace mh
{
	template<typename T>
	class property
	{
	public:
		constexpr property() = default;
		constexpr explicit property(const T& value) : m_Value(value) {}
		constexpr explicit property(T&& value) : m_Value(std::move(value)) {}

		constexpr const T& get() const { return m_Value; }
		constexpr operator const T& () const { return get(); }

		constexpr property& operator=(const property& rhs) { return set(rhs); }
		constexpr property& operator=(const T& rhs) { return set(rhs); }
		constexpr property& operator=(T&& rhs) { return set(std::move(rhs)); }

		constexpr property& set(const T& newValue)
		{
			if (newValue != m_Value && OnChange(newValue))
				m_Value = newValue;

			return *this;
		}
		constexpr property& set(T&& newValue)
		{
			if (newValue != m_Value && OnChange(newValue))
				m_Value = std::move(newValue);

			return *this;
		}

		constexpr auto operator<=>(const property&) const = default;

	protected:
		virtual bool OnChange(const T& newValue) const { return true; }

	private:
		T m_Value;
	};

	template<typename T> constexpr auto operator<=>(const property<T>& lhs, const T& rhs) { return lhs.get() <=> rhs; }
	template<typename T> constexpr auto operator<=>(const T& lhs, const property<T>& rhs) { return lhs <=> rhs.get(); }
	template<typename T> constexpr auto operator==(const property<T>& lhs, const T& rhs) { return lhs.get() == rhs; }
	template<typename T> constexpr bool operator==(const T& lhs, const property<T>& rhs) { return lhs == rhs.get(); }
}

static constexpr const char DEFAULT_LARGE_IMAGE_KEY[] = "tf2_1x1";

static constexpr LogMessageColor DISCORD_LOG_COLOR{ 117 / 255.0f, 136 / 255.0f, 215 / 255.0f };

static bool s_DiscordDebugLogEnabled = true;

static void DiscordDebugLog(const std::string_view& msg)
{
	if (!s_DiscordDebugLogEnabled)
		return;

	DebugLog(DISCORD_LOG_COLOR, "DRP: {}", msg);
}
static void DiscordDebugLog(const mh::source_location& location, const std::string_view& msg = {})
{
	if (!s_DiscordDebugLogEnabled)
		return;

	if (msg.empty())
		DebugLog(DISCORD_LOG_COLOR, location);
	else
		DebugLog(DISCORD_LOG_COLOR, location, "DRP: {}", msg);
}

namespace
{
	enum class ConnectionState
	{
		Disconnected,  // We're not connected to a server in the first place.
		Local,         // This is a local (loopback) server.
		Nonlocal,      // This is a non-local server.
	};
}

#undef OS_CASE
#define OS_CASE(v) case v : return os << #v
template<typename CharT, typename Traits>
static std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, discord::Result result)
{
	using Result = discord::Result;

	switch (result)
	{
		OS_CASE(Result::Ok);
		OS_CASE(Result::ServiceUnavailable);
		OS_CASE(Result::InvalidVersion);
		OS_CASE(Result::LockFailed);
		OS_CASE(Result::InternalError);
		OS_CASE(Result::InvalidPayload);
		OS_CASE(Result::InvalidCommand);
		OS_CASE(Result::InvalidPermissions);
		OS_CASE(Result::NotFetched);
		OS_CASE(Result::NotFound);
		OS_CASE(Result::Conflict);
		OS_CASE(Result::InvalidSecret);
		OS_CASE(Result::InvalidJoinSecret);
		OS_CASE(Result::NoEligibleActivity);
		OS_CASE(Result::InvalidInvite);
		OS_CASE(Result::NotAuthenticated);
		OS_CASE(Result::InvalidAccessToken);
		OS_CASE(Result::ApplicationMismatch);
		OS_CASE(Result::InvalidDataUrl);
		OS_CASE(Result::InvalidBase64);
		OS_CASE(Result::NotFiltered);
		OS_CASE(Result::LobbyFull);
		OS_CASE(Result::InvalidLobbySecret);
		OS_CASE(Result::InvalidFilename);
		OS_CASE(Result::InvalidFileSize);
		OS_CASE(Result::InvalidEntitlement);
		OS_CASE(Result::NotInstalled);
		OS_CASE(Result::NotRunning);
		OS_CASE(Result::InsufficientBuffer);
		OS_CASE(Result::PurchaseCanceled);
		OS_CASE(Result::InvalidGuild);
		OS_CASE(Result::InvalidEvent);
		OS_CASE(Result::InvalidChannel);
		OS_CASE(Result::InvalidOrigin);
		OS_CASE(Result::RateLimited);
		OS_CASE(Result::OAuth2Error);
		OS_CASE(Result::SelectChannelTimeout);
		OS_CASE(Result::GetGuildTimeout);
		OS_CASE(Result::SelectVoiceForceRequired);
		OS_CASE(Result::CaptureShortcutAlreadyListening);
		OS_CASE(Result::UnauthorizedForAchievement);
		OS_CASE(Result::InvalidGiftCode);
		OS_CASE(Result::PurchaseError);
		OS_CASE(Result::TransactionAborted);

	default:
		return os << "Result(" << +std::underlying_type_t<Result>(result) << ')';
	}
}
template<typename CharT, typename Traits>
static std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, discord::ActivityType type)
{
	using ActivityType = discord::ActivityType;
	switch (type)
	{
		OS_CASE(ActivityType::Playing);
		OS_CASE(ActivityType::Streaming);
		OS_CASE(ActivityType::Listening);
		OS_CASE(ActivityType::Watching);

	default:
		return os << "ActivityType(" << +std::underlying_type_t<ActivityType>(type) << ')';
	}
}

template<typename CharT, typename Traits>
static std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, ConnectionState cs)
{
	switch (cs)
	{
		OS_CASE(ConnectionState::Disconnected);
		OS_CASE(ConnectionState::Local);
		OS_CASE(ConnectionState::Nonlocal);

	default:
		return os << "ConnectionState(" << +std::underlying_type_t<ConnectionState>(cs) << ')';
	}
}
#undef OS_CASE

static std::strong_ordering strcmp3(const char* lhs, const char* rhs)
{
	const auto result = strcmp(lhs, rhs);
	if (result < 0)
		return std::strong_ordering::less;
	else if (result == 0)
		return std::strong_ordering::equal;
	else //if (result > 0)
		return std::strong_ordering::greater;
}

static std::strong_ordering operator<=>(const discord::ActivityTimestamps& lhs, const discord::ActivityTimestamps& rhs)
{
	if (auto result = lhs.GetStart() <=> rhs.GetStart(); result != 0)
		return result;
	if (auto result = lhs.GetEnd() <=> rhs.GetEnd(); result != 0)
		return result;

	return std::strong_ordering::equal;
}

static std::strong_ordering operator<=>(const discord::ActivityAssets& lhs, const discord::ActivityAssets& rhs)
{
	if (auto result = strcmp3(lhs.GetLargeImage(), rhs.GetLargeImage()); result != 0)
		return result;
	if (auto result = strcmp3(lhs.GetLargeText(), rhs.GetLargeText()); result != 0)
		return result;
	if (auto result = strcmp3(lhs.GetSmallImage(), rhs.GetSmallImage()); result != 0)
		return result;
	if (auto result = strcmp3(lhs.GetSmallText(), rhs.GetSmallText()); result != 0)
		return result;

	return std::strong_ordering::equal;
}

static std::strong_ordering operator<=>(const discord::PartySize& lhs, const discord::PartySize& rhs)
{
	if (auto result = lhs.GetCurrentSize() <=> rhs.GetCurrentSize(); result != 0)
		return result;
	if (auto result = lhs.GetMaxSize() <=> rhs.GetMaxSize(); result != 0)
		return result;

	return std::strong_ordering::equal;
}

static std::strong_ordering operator<=>(const discord::ActivityParty& lhs, const discord::ActivityParty& rhs)
{
	if (auto result = strcmp3(lhs.GetId(), rhs.GetId()); result != 0)
		return result;
	if (auto result = lhs.GetSize() <=> rhs.GetSize(); result != 0)
		return result;

	return std::strong_ordering::equal;
}

static std::strong_ordering operator<=>(const discord::ActivitySecrets& lhs, const discord::ActivitySecrets& rhs)
{
	if (auto result = strcmp3(lhs.GetMatch(), rhs.GetMatch()); result != 0)
		return result;
	if (auto result = strcmp3(lhs.GetJoin(), rhs.GetJoin()); result != 0)
		return result;
	if (auto result = strcmp3(lhs.GetSpectate(), rhs.GetSpectate()); result != 0)
		return result;

	return std::strong_ordering::equal;
}

static std::strong_ordering operator<=>(const discord::Activity& lhs, const discord::Activity& rhs)
{
	if (auto result = lhs.GetType() <=> rhs.GetType(); result != 0)
		return result;
	if (auto result = lhs.GetApplicationId() <=> rhs.GetApplicationId(); result != 0)
		return result;
	if (auto result = strcmp3(lhs.GetName(), rhs.GetName()); result != 0)
		return result;
	if (auto result = strcmp3(lhs.GetState(), rhs.GetState()); result != 0)
		return result;
	if (auto result = strcmp3(lhs.GetDetails(), rhs.GetDetails()); result != 0)
		return result;
	if (auto result = lhs.GetTimestamps() <=> rhs.GetTimestamps(); result != 0)
		return result;
	if (auto result = lhs.GetAssets() <=> rhs.GetAssets(); result != 0)
		return result;
	if (auto result = lhs.GetParty() <=> rhs.GetParty(); result != 0)
		return result;
	if (auto result = lhs.GetSecrets() <=> rhs.GetSecrets(); result != 0)
		return result;
	if (auto result = lhs.GetInstance() <=> rhs.GetInstance(); result != 0)
		return result;

	return std::strong_ordering::equal;
}

template<typename CharT, typename Traits>
static std::basic_ostream<CharT, Traits>& operator<<(
	std::basic_ostream<CharT, Traits>& os, const discord::ActivityTimestamps& ts)
{
	return os
		<< "\n\t\tStart: " << ts.GetStart()
		<< "\n\t\tEnd:   " << ts.GetEnd()
		;
}

template<typename CharT, typename Traits>
static std::basic_ostream<CharT, Traits>& operator<<(
	std::basic_ostream<CharT, Traits>& os, const discord::ActivityAssets& a)
{
	return os
		<< "\n\t\tLargeImage: " << std::quoted(a.GetLargeImage())
		<< "\n\t\tLargeText:  " << std::quoted(a.GetLargeText())
		<< "\n\t\tSmallImage: " << std::quoted(a.GetSmallImage())
		<< "\n\t\tSmallText:  " << std::quoted(a.GetSmallText())
		;
}

template<typename CharT, typename Traits>
static std::basic_ostream<CharT, Traits>& operator<<(
	std::basic_ostream<CharT, Traits>& os, const discord::PartySize& ps)
{
	return os
		<< "\n\t\t\tCurrent: " << ps.GetCurrentSize()
		<< "\n\t\t\tMax:     " << ps.GetMaxSize()
		;
}

template<typename CharT, typename Traits>
static std::basic_ostream<CharT, Traits>& operator<<(
	std::basic_ostream<CharT, Traits>& os, const discord::ActivityParty& p)
{
	return os
		<< "\n\t\tID:   " << std::quoted(p.GetId())
		<< "\n\t\tSize: " << p.GetSize()
		;
}

template<typename CharT, typename Traits>
static std::basic_ostream<CharT, Traits>& operator<<(
	std::basic_ostream<CharT, Traits>& os, const discord::ActivitySecrets& s)
{
	return os
		<< "\n\t\tMatch:    " << std::quoted(s.GetMatch())
		<< "\n\t\tJoin:     " << std::quoted(s.GetJoin())
		<< "\n\t\tSpectate: " << std::quoted(s.GetSpectate())
		;
}

template<typename CharT, typename Traits>
static std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, const discord::Activity& a)
{
	return os << std::boolalpha
		<< "\n\tType:          " << a.GetType()
		<< "\n\tApplicationID: " << a.GetApplicationId()
		<< "\n\tName:          " << std::quoted(a.GetName())
		<< "\n\tState:         " << std::quoted(a.GetState())
		<< "\n\tDetails:       " << std::quoted(a.GetDetails())
		<< "\n\tTimestamps:    " << a.GetTimestamps()
		<< "\n\tAssets:        " << a.GetAssets()
		<< "\n\tParty:         " << a.GetParty()
		<< "\n\tSecrets:       " << a.GetSecrets()
		<< "\n\tInstance:      " << a.GetInstance()
		;
}

namespace
{
	struct DiscordGameState final
	{
		DiscordGameState(const Settings& settings, const DRPInfo& drpInfo) : m_Settings(&settings), m_DRPInfo(&drpInfo) {}

		discord::Activity ConstructActivity() const;

		void OnQueueStateChange(TFMatchGroup queueType, TFQueueStateChange state);
		void OnQueueStatusUpdate(TFMatchGroup queueType, time_point_t queueStartTime);
		void OnServerIPUpdate(const std::string_view& localIP);
		void SetInLobby(bool inLobby);
		void SetInLocalServer(bool inLocalServer);
		void SetInParty(bool inParty);
		void SetMapName(std::string mapName);
		void UpdateParty(uint8_t partyMembers);
		void OnLocalPlayerSpawned(TFClassType classType);
		void OnConnectionCountUpdate(unsigned connectionCount);

	private:
		const Settings* m_Settings = nullptr;
		const DRPInfo* m_DRPInfo = nullptr;

		struct QueueState
		{
			bool m_Active = false;
			time_point_t m_StartTime{};
		};
		std::array<QueueState, size_t(TFMatchGroup::COUNT)> m_QueueStates;

		bool IsQueueActive(TFMatchGroup matchGroup) const { return m_QueueStates.at((int)matchGroup).m_Active; }
		bool IsAnyQueueActive(TFMatchGroupFlags matchGroups) const;
		bool IsAnyQueueActive() const;

		std::optional<time_point_t> GetEarliestActiveQueueStartTime() const;

		TFClassType m_LastSpawnedClass = TFClassType::Undefined;
		std::string m_MapName;
		bool m_InLobby = false;
		uint8_t m_PartyMemberCount = 0;
		time_point_t m_LastStatusTimestamp;

		struct : public mh::property<ConnectionState>
		{
			using BaseClass = mh::property<ConnectionState>;
			using BaseClass::operator=;

			bool OnChange(const ConnectionState& newValue) const override
			{
				DiscordDebugLog("ConnectionState "s << get() << " -> " << newValue);
				return true;
			}

		} m_ConnectionState;
	};
}

bool DiscordGameState::IsAnyQueueActive(TFMatchGroupFlags matchGroups) const
{
	for (size_t i = 0; i < size_t(TFMatchGroup::COUNT); i++)
	{
		if (!!(matchGroups & TFMatchGroup(i)) && IsQueueActive(TFMatchGroup(i)))
			return true;
	}

	return false;
}

bool DiscordGameState::IsAnyQueueActive() const
{
	for (const auto& type : m_QueueStates)
	{
		if (type.m_Active)
			return true;
	}

	return false;
}

std::optional<time_point_t> DiscordGameState::GetEarliestActiveQueueStartTime() const
{
	std::optional<time_point_t> retVal{};

	for (const auto& type : m_QueueStates)
	{
		if (!type.m_Active)
			continue;

		if (!retVal)
			retVal = type.m_StartTime;
		else
			retVal = std::min(*retVal, type.m_StartTime);
	}

	return retVal;
}

discord::Activity DiscordGameState::ConstructActivity() const
{
	discord::Activity retVal{};

	std::string details;

	if (m_ConnectionState != ConnectionState::Disconnected && !m_MapName.empty())
	{
		if (auto map = m_DRPInfo->FindMap(m_MapName))
			retVal.GetAssets().SetLargeImage(mh::format("map_{}", map->m_MapNames.at(0)).c_str());
		else
			retVal.GetAssets().SetLargeImage("map_unknown");

		details = m_MapName;
	}
	else
	{
		retVal.GetAssets().SetLargeImage(DEFAULT_LARGE_IMAGE_KEY);
	}

	const auto GetGameState = [&]
	{
		if (m_ConnectionState == ConnectionState::Local)
		{
			return "Local Server";
		}
		else if (m_ConnectionState == ConnectionState::Nonlocal)
		{
			if (m_InLobby)
				return "Casual";
			else
				return "Community";
		}
		else if (m_ConnectionState == ConnectionState::Disconnected)
		{
			return "Main Menu";
		}

		return "";
	};

	if (IsAnyQueueActive())
	{
		const bool isCasual = IsAnyQueueActive(TFMatchGroupFlags::Casual);
		const bool isMVM = IsAnyQueueActive(TFMatchGroupFlags::MVM);
		const bool isComp = IsAnyQueueActive(TFMatchGroupFlags::Competitive);

		if (isCasual && isComp && isMVM)
			retVal.SetState("Searching - Casual, Competitive, and MVM");
		else if (isCasual && isComp)
			retVal.SetState("Searching - Casual & Competitive");
		else if (isCasual && isMVM)
			retVal.SetState("Searching - Casual & MVM");
		else if (isCasual)
			retVal.SetState("Searching - Casual");
		else if (isComp && isMVM)
			retVal.SetState("Searching - Competitive & MVM");
		else if (isComp)
			retVal.SetState("Searching - Competitive");
		else if (isMVM)
			retVal.SetState("Searching - MVM");
		else
		{
			assert(!"Unknown combination of flags");
			retVal.SetState("Searching");
		}

		if (auto startTime = GetEarliestActiveQueueStartTime())
			retVal.GetTimestamps().SetStart(to_seconds<discord::Timestamp>(startTime->time_since_epoch()));

		if (m_ConnectionState != ConnectionState::Disconnected)
		{
			if (details.empty())
				details = GetGameState();
			else
				details = mh::format("{} - {}", GetGameState(), details);
		}
	}
	else
	{
		retVal.SetState(GetGameState());
	}

	if (m_PartyMemberCount > 0)
	{
		retVal.GetParty().GetSize().SetCurrentSize(m_PartyMemberCount);
		retVal.GetParty().GetSize().SetMaxSize(6);
	}

	if (m_ConnectionState != ConnectionState::Disconnected)
	{
		auto& assets = retVal.GetAssets();
		switch (m_LastSpawnedClass)
		{
		case TFClassType::Demoman:
			assets.SetSmallImage("leaderboard_class_demo");
			assets.SetSmallText("Demo");
			break;
		case TFClassType::Engie:
			assets.SetSmallImage("leaderboard_class_engineer");
			assets.SetSmallText("Engineer");
			break;
		case TFClassType::Heavy:
			assets.SetSmallImage("leaderboard_class_heavy");
			assets.SetSmallText("Heavy");
			break;
		case TFClassType::Medic:
			assets.SetSmallImage("leaderboard_class_medic");
			assets.SetSmallText("Medic");
			break;
		case TFClassType::Pyro:
			assets.SetSmallImage("leaderboard_class_pyro");
			assets.SetSmallText("Pyro");
			break;
		case TFClassType::Scout:
			assets.SetSmallImage("leaderboard_class_scout");
			assets.SetSmallText("Scout");
			break;
		case TFClassType::Sniper:
			assets.SetSmallImage("leaderboard_class_sniper");
			assets.SetSmallText("Sniper");
			break;
		case TFClassType::Soldier:
			assets.SetSmallImage("leaderboard_class_soldier");
			assets.SetSmallText("Soldier");
			break;
		case TFClassType::Spy:
			assets.SetSmallImage("leaderboard_class_spy");
			assets.SetSmallText("Spy");
			break;
		}
	}

	retVal.SetDetails(details.c_str());

	return retVal;
}

void DiscordGameState::OnQueueStateChange(TFMatchGroup queueType, TFQueueStateChange state)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	auto& queue = m_QueueStates.at(size_t(queueType));

	if (state == TFQueueStateChange::Entered)
	{
		queue.m_Active = true;
		queue.m_StartTime = clock_t::now();
	}
	else if (state == TFQueueStateChange::Exited || state == TFQueueStateChange::RequestedExit)
	{
		queue.m_Active = false;
	}
}

void DiscordGameState::OnQueueStatusUpdate(TFMatchGroup queueType, time_point_t queueStartTime)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	auto& queue = m_QueueStates.at(size_t(queueType));
	queue.m_Active = true;
	queue.m_StartTime = queueStartTime;
}

void DiscordGameState::OnServerIPUpdate(const std::string_view& localIP)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	if (localIP.starts_with("0.0.0.0:"))
	{
		SetInLocalServer(true);
	}
	else
	{
		SetInLocalServer(false);
		m_ConnectionState = ConnectionState::Nonlocal;
	}
}

void DiscordGameState::SetInLobby(bool inLobby)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	if (inLobby)
		m_ConnectionState = ConnectionState::Nonlocal;

	m_InLobby = inLobby;
}

void DiscordGameState::SetInLocalServer(bool inLocalServer)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	if (inLocalServer)
	{
		m_ConnectionState = ConnectionState::Local;
		m_InLobby = false;
	}
}

void DiscordGameState::SetInParty(bool inParty)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	if (inParty)
	{
		if (m_PartyMemberCount < 1)
			m_PartyMemberCount = 1;
	}
	else
	{
		m_PartyMemberCount = 0;
	}
}

void DiscordGameState::SetMapName(std::string mapName)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	m_MapName = std::move(mapName);
	if (m_MapName.empty())
		m_LastSpawnedClass = TFClassType::Undefined;
}

void DiscordGameState::UpdateParty(uint8_t partyMembers)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	if (partyMembers > 0)
	{
		SetInParty(true);
		m_PartyMemberCount = partyMembers;
	}
}

void DiscordGameState::OnLocalPlayerSpawned(TFClassType classType)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	m_LastSpawnedClass = classType;
}

void DiscordGameState::OnConnectionCountUpdate(unsigned connectionCount)
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
	if (connectionCount < 1)
		m_ConnectionState = ConnectionState::Disconnected;
}

namespace
{
	class DiscordState final : public IDRPManager, AutoWorldEventListener, AutoConsoleLineListener
	{
	public:
		static constexpr duration_t UPDATE_INTERVAL = 10s;
		static_assert(UPDATE_INTERVAL >= (20s / 5), "Update interval too low");

		DiscordState(const Settings& settings, IWorldState& world);
		~DiscordState();

		void QueueUpdate() { m_WantsUpdate = true; }
		void Update() override;

		void OnConsoleLineParsed(IWorldState& world, IConsoleLine& line) override;
		void OnLocalPlayerSpawned(IWorldState& world, TFClassType classType) override;

	private:
		std::unique_ptr<discord::Core> m_Core;
		time_point_t m_LastDiscordInitializeTime{};

		const Settings& GetSettings() const { return m_Settings; }
		IWorldState& GetWorld() { return m_WorldState; }

		const Settings& m_Settings;
		IWorldState& m_WorldState;
		DRPInfo m_DRPInfo;
		DiscordGameState m_GameState;

		bool m_WantsUpdate = false;
		time_point_t m_LastUpdate{};
		discord::Activity m_CurrentActivity{};
	};
}

DiscordState::DiscordState(const Settings& settings, IWorldState& world) :
	AutoWorldEventListener(world),
	AutoConsoleLineListener(world),
	m_Settings(settings),
	m_WorldState(world),
	m_GameState(settings, m_DRPInfo),
	m_DRPInfo(settings)
{
}

DiscordState::~DiscordState()
{
	DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT());
}

void DiscordState::OnConsoleLineParsed(IWorldState& world, IConsoleLine& line)
{
	switch (line.GetType())
	{
	case ConsoleLineType::PlayerStatusMapPosition:
	{
		QueueUpdate();
		auto& statusLine = static_cast<const ServerStatusMapLine&>(line);
		m_GameState.SetMapName(statusLine.GetMapName());
		break;
	}
	case ConsoleLineType::PartyHeader:
	{
		QueueUpdate();
		auto& partyLine = static_cast<const PartyHeaderLine&>(line);
		m_GameState.UpdateParty(partyLine.GetParty().m_MemberCount);
		break;
	}
	case ConsoleLineType::LobbyHeader:
	{
		QueueUpdate();
		m_GameState.SetInLobby(true);
		break;
	}
	case ConsoleLineType::LobbyStatusFailed:
	{
		QueueUpdate();
		m_GameState.SetInLobby(false);
		break;
	}
	case ConsoleLineType::QueueStateChange:
	{
		QueueUpdate();
		auto& queueLine = static_cast<const QueueStateChangeLine&>(line);
		m_GameState.OnQueueStateChange(queueLine.GetQueueType(), queueLine.GetStateChange());
		break;
	}
	case ConsoleLineType::InQueue:
	{
		QueueUpdate();
		auto& queueLine = static_cast<const InQueueLine&>(line);
		m_GameState.OnQueueStatusUpdate(queueLine.GetQueueType(), queueLine.GetQueueStartTime());
		break;
	}
	case ConsoleLineType::LobbyChanged:
	{
		QueueUpdate();
		auto& lobbyLine = static_cast<const LobbyChangedLine&>(line);
		if (lobbyLine.GetChangeType() == LobbyChangeType::Destroyed)
			m_GameState.SetMapName("");

		break;
	}
	case ConsoleLineType::ServerJoin:
	{
		QueueUpdate();
		auto& joinLine = static_cast<const ServerJoinLine&>(line);
		m_GameState.SetMapName(joinLine.GetMapName());
		// Not necessarily in a lobby at this point, but in-lobby state will be reapplied soon if we are in a lobby
		m_GameState.SetInLobby(false);
		break;
	}
	case ConsoleLineType::HostNewGame:
	{
		QueueUpdate();
		m_GameState.SetInLocalServer(true);
		break;
	}
	case ConsoleLineType::PlayerStatusIP:
	{
		QueueUpdate();
		auto& ipLine = static_cast<const ServerStatusPlayerIPLine&>(line);
		m_GameState.OnServerIPUpdate(ipLine.GetLocalIP());
		break;
	}
	case ConsoleLineType::NetStatusConfig:
	{
		QueueUpdate();
		auto& netLine = static_cast<const NetStatusConfigLine&>(line);
		m_GameState.OnConnectionCountUpdate(netLine.GetConnectionCount());
		break;
	}
	case ConsoleLineType::Connecting:
	{
		QueueUpdate();
		auto& connLine = static_cast<const ConnectingLine&>(line);
		m_GameState.OnServerIPUpdate(connLine.GetAddress());
		break;
	}
	case ConsoleLineType::SVC_UserMessage:
	{
		QueueUpdate();
		auto& umsgLine = static_cast<SVCUserMessageLine&>(line);
		m_GameState.OnServerIPUpdate(umsgLine.GetAddress());
		break;
	}
	}
}

void DiscordState::OnLocalPlayerSpawned(IWorldState& world, TFClassType classType)
{
	QueueUpdate();
	m_GameState.OnLocalPlayerSpawned(classType);
}

static void DiscordLogFunc(discord::LogLevel level, const char* msg)
{
	switch (level)
	{
	case discord::LogLevel::Debug:
		DebugLog(msg);
		break;
	case discord::LogLevel::Info:
		Log(msg);
		break;
	case discord::LogLevel::Warn:
		LogWarning(msg);
		break;

	default:
		LogError("Unknown discord LogLevel "s << +std::underlying_type_t<discord::LogLevel>(level));
		[[fallthrough]];
	case discord::LogLevel::Error:
		LogError(msg);
		break;
	}
}

void DiscordState::Update()
{
	s_DiscordDebugLogEnabled = GetSettings().m_Logging.m_DiscordRichPresence;

	// Initialize discord
	const auto curTime = clock_t::now();
	if (!m_Core && (curTime - m_LastDiscordInitializeTime) > 10s)
	{
		m_LastDiscordInitializeTime = curTime;

		discord::Core* core = nullptr;
		if (auto result = discord::Core::Create(730945386390224976, DiscordCreateFlags_NoRequireDiscord, &core);
			result != discord::Result::Ok)
		{
			LogError("Failed to initialize Discord Game SDK: "s << result);
		}
		else
		{
			m_Core.reset(core);

			if (auto result = m_Core->ActivityManager().RegisterSteam(440); result != discord::Result::Ok)
				LogError("Failed to register discord integration as steam appid 440: "s << result);
		}
	}

	if (m_Core)
	{
		std::invoke([&]()
			{
				if (!m_WantsUpdate)
					return;

				if ((curTime - m_LastUpdate) < UPDATE_INTERVAL)
					return;

				if (const auto nextActivity = m_GameState.ConstructActivity();
					std::is_neq(nextActivity <=> m_CurrentActivity))
				{
					// Perform the update
					m_Core->ActivityManager().UpdateActivity(nextActivity, [nextActivity](discord::Result result)
						{
							if (result == discord::Result::Ok)
							{
								DiscordDebugLog("Updated discord activity state: "s << nextActivity);
							}
							else
							{
								LogWarning(MH_SOURCE_LOCATION_CURRENT(),
									"Failed to update discord activity state: "s << result);
							}
						});

					m_CurrentActivity = nextActivity;
					m_LastUpdate = curTime;
				}
				else
				{
					DiscordDebugLog(MH_SOURCE_LOCATION_CURRENT(), "Discord activity state unchanged");
				}

				// Update successful
				m_WantsUpdate = false;
			});

		// Run discord callbacks
		if (auto result = m_Core->RunCallbacks(); result != discord::Result::Ok)
		{
			auto errMsg = mh::format("Failed to run discord callbacks: {}", result);
			switch (result)
			{
			case discord::Result::NotRunning:
				DebugLogWarning(std::move(errMsg));
				// Discord Game SDK will never recover from this state. Need to shutdown and try
				// to reinitialize in a bit. Also set the last initialize time to now, so we don't
				// try to reinitialize next frame (which will probably fail).
				m_Core.reset();
				m_LastDiscordInitializeTime = curTime;
				break;

			default:
				LogError(std::move(errMsg));
				break;
			}
		}
	}
}

#endif

std::unique_ptr<IDRPManager> IDRPManager::Create(const Settings& settings, IWorldState& world)
{
	return std::make_unique<DiscordState>(settings, world);
}
