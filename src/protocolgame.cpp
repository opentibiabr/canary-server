/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2020  Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "otpch.h"

#include "protocolgame.h"

#include "modules.h"
#include "outputmessage.h"

#include "player.h"
#include "monsters.h"

#include "databasetasks.h"
#include "configmanager.h"
#include "actions.h"
#include "game.h"
#include "iologindata.h"
#include "iomarket.h"
#include "waitlist.h"
#include "ban.h"
#include "spells.h"

extern Actions actions;

NetworkMessage ProtocolGame::playermsg;

void ProtocolGame::release()
{
	//dispatcher thread
	if (player && player->client == shared_from_this()) {
		player->client.reset();
		player->decrementReferenceCounter();
		player = nullptr;
	}

	OutputMessagePool::getInstance().removeProtocolFromAutosend(shared_from_this());
	Protocol::release();
}

#if GAME_FEATURE_SESSIONKEY > 0
void ProtocolGame::login(const std::string& accountName, const std::string& password, std::string& characterName, std::string& token, uint32_t tokenTime, OperatingSystem_t operatingSystem, OperatingSystem_t tfcOperatingSystem)
#else
void ProtocolGame::login(const std::string& accountName, const std::string& password, std::string& characterName, OperatingSystem_t operatingSystem, OperatingSystem_t tfcOperatingSystem)
#endif
{
	//dispatcher thread
	BanInfo banInfo;
	if (IOBan::isIpBanned(getIP(), banInfo)) {
		if (banInfo.reason.empty()) {
			banInfo.reason = "(none)";
		}

		std::ostringstream ss;
		ss << "Your IP has been banned until " << formatDateShort(banInfo.expiresAt) << " by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
		disconnectClient(ss.str());
		return;
	}
	
	#if GAME_FEATURE_SESSIONKEY > 0
	uint32_t accountId = IOLoginData::gameworldAuthentication(accountName, password, characterName, token, tokenTime);
	if (accountId == 0) {
		disconnectClient("Account name or password is not correct.");
		return;
	}
	#else
	uint32_t accountId = IOLoginData::gameworldAuthentication(accountName, password, characterName);
	if (accountId == 0) {
		disconnectClient("Account name or password is not correct.");
		return;
	}
	#endif

	Player* foundPlayer = g_game().getPlayerByName(characterName);
	if (!foundPlayer || g_config().getBoolean(ConfigManager::ALLOW_CLONES)) {
		player = new Player(getThis());
		player->setName(characterName);

		player->incrementReferenceCounter();

		if (!IOLoginData::preloadPlayer(player, characterName)) {
			disconnectClient("Your character could not be loaded.");
			return;
		}

		player->setID();
		if (IOBan::isPlayerNamelocked(player->getGUID())) {
			disconnectClient("Your character has been namelocked.");
			return;
		}

		if (g_game().getGameState() == GAME_STATE_CLOSING && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
			disconnectClient("The game is just going down.\nPlease try again later.");
			return;
		}

		if (g_game().getGameState() == GAME_STATE_CLOSED && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
			disconnectClient("Server is currently closed.\nPlease try again later.");
			return;
		}

		if (g_config().getBoolean(ConfigManager::ONE_PLAYER_ON_ACCOUNT) && player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER && g_game().getPlayerByAccount(player->getAccount())) {
			disconnectClient("You may only login with one character\nof your account at the same time.");
			return;
		}

		if (!player->hasFlag(PlayerFlag_CannotBeBanned)) {
			if (IOBan::isAccountBanned(accountId, banInfo)) {
				if (banInfo.reason.empty()) {
					banInfo.reason = "(none)";
				}

				std::ostringstream ss;
				if (banInfo.expiresAt > 0) {
					ss << "Your account has been banned until " << formatDateShort(banInfo.expiresAt) << " by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
				} else {
					ss << "Your account has been permanently banned by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
				}
				disconnectClient(ss.str());
				return;
			}
		}

		std::size_t currentSlot;
		if (!WaitingList::getInstance().clientLogin(player, currentSlot)) {
			int64_t retryTime = WaitingList::getTime(currentSlot);
			std::ostringstream ss;

			ss << "Too many players online.\nYou are at place "
			   << currentSlot << " on the waiting list.";

			auto output = OutputMessagePool::getOutputMessage();
			output->addByte(0x16);
			output->addString(ss.str());
			output->addByte(static_cast<uint8_t>(retryTime));
			send(output);
			disconnect();
			return;
		}

		if (!IOLoginData::loadPlayerById(player, player->getGUID())) {
			disconnectClient("Your character could not be loaded.");
			return;
		}
		
		player->setOperatingSystem(operatingSystem);
		player->setTfcOperatingSystem(tfcOperatingSystem);
		if (!g_game().placeCreature(player, player->getLoginPosition())) {
			if (!g_game().placeCreature(player, player->getTemplePosition(), false, true)) {
				disconnectClient("Temple position is wrong. Contact the administrator.");
				return;
			}
		}

		if (operatingSystem >= CLIENTOS_OTCLIENT_LINUX) {
			NetworkMessage opcodeMessage;
			opcodeMessage.addByte(0x32);
			opcodeMessage.addByte(0x00);
			opcodeMessage.add<uint16_t>(0x00);
			writeToOutputBuffer(opcodeMessage);

			player->registerCreatureEvent("ExtendedOpcode");
		}

		player->lastIP = player->getIP();
		player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
		acceptPackets = true;
	} else {
		if (eventConnect != 0 || !g_config().getBoolean(ConfigManager::REPLACE_KICK_ON_LOGIN)) {
			//Already trying to connect
			disconnectClient("You are already logged in.");
			return;
		}

		if (foundPlayer->client) {
			foundPlayer->disconnect();
			foundPlayer->isConnecting = true;

			eventConnect = g_dispatcher().addEvent(1000, std::bind(&ProtocolGame::connect, getThis(), foundPlayer->getID(), operatingSystem, tfcOperatingSystem));
		} else {
			connect(foundPlayer->getID(), operatingSystem, tfcOperatingSystem);
		}
	}
	OutputMessagePool::getInstance().addProtocolToAutosend(shared_from_this());
}

void ProtocolGame::connect(uint32_t playerId, OperatingSystem_t operatingSystem, OperatingSystem_t tfcOperatingSystem)
{
	eventConnect = 0;

	if (isConnectionExpired()) {
		//ProtocolGame::release() has been called at this point and the Connection object
		//no longer exists, so we return to prevent leakage of the Player.
		return;
	}

	Player* foundPlayer = g_game().getPlayerByID(playerId);
	if (!foundPlayer || foundPlayer->client) {
		disconnectClient("You are already logged in.");
		return;
	}

	player = foundPlayer;
	player->incrementReferenceCounter();

	g_chat().removeUserFromAllChannels(*player);
	player->clearModalWindows();
	player->setOperatingSystem(operatingSystem);
	player->setTfcOperatingSystem(tfcOperatingSystem);
	player->isConnecting = false;

	player->client = getThis();
	sendAddCreature(player, player->getPosition(), 0, false);
	g_chat().openChannelsByServer(player);
	player->lastIP = player->getIP();
	player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
	acceptPackets = true;
}

void ProtocolGame::logout(bool displayEffect, bool forced)
{
	//dispatcher thread
	if (!player) {
		return;
	}

	if (!player->isRemoved()) {
		if (!forced) {
			if (!player->isAccessPlayer()) {
				if (player->getTile()->hasFlag(TILESTATE_NOLOGOUT)) {
					player->sendCancelMessage(RETURNVALUE_YOUCANNOTLOGOUTHERE);
					return;
				}

				if (!player->getTile()->hasFlag(TILESTATE_PROTECTIONZONE) && player->hasCondition(CONDITION_INFIGHT)) {
					player->sendCancelMessage(RETURNVALUE_YOUMAYNOTLOGOUTDURINGAFIGHT);
					return;
				}
			}

			//scripting event - onLogout
			if (!g_creatureEvents().playerLogout(player)) {
				//Let the script handle the error message
				return;
			}
		}

		if (displayEffect && player->getHealth() > 0) {
			g_game().addMagicEffect(player->getPosition(), CONST_ME_POFF);
		}
	}

	disconnect();

	g_game().removeCreature(player);
}

void ProtocolGame::onRecvFirstMessage(NetworkMessage& msg)
{
	if (g_game().getGameState() == GAME_STATE_SHUTDOWN) {
		disconnect();
		return;
	}

	OperatingSystem_t operatingSystem = static_cast<OperatingSystem_t>(msg.getByte());
	OperatingSystem_t TFCoperatingSystem = static_cast<OperatingSystem_t>(msg.getByte());
	msg.get<uint16_t>();
	// if it's not OTC use SEQUENCE checksum
	if (operatingSystem < CLIENTOS_OTCLIENT_LINUX) {
		setChecksumMethod(CHECKSUM_METHOD_SEQUENCE);
		if (TFCoperatingSystem < CLIENTOS_TFC_ANDROID) {
			//compression on the forgotten client will be implemented in the next client update
			enableCompression();
		}
	} else { // if it's OTC use ADLER32 checksum
		setChecksumMethod(CHECKSUM_METHOD_ADLER32);
	}

	#if GAME_FEATURE_CLIENT_VERSION > 0
	msg.skipBytes(4);
	#endif
	#if GAME_FEATURE_CONTENT_REVISION > 0
	msg.skipBytes(2);
	#endif
	#if GAME_FEATURE_PREVIEW_STATE > 0
	msg.skipBytes(1);
	#endif
	#if GAME_FEATURE_CLIENT_VERSION > 0
	// if (msg.getLength() - msg.getBufferPosition() > 128) {
	// 	addExivaRestrictions = true;
	// 	msg.skipBytes(1);
	// }
	#endif

	#if GAME_FEATURE_RSA1024 > 0
	if (!Protocol::RSA_decrypt(msg)) {
		disconnect();
		return;
	}
	#endif

	#if GAME_FEATURE_XTEA > 0
	uint32_t key[4] = {msg.get<uint32_t>(), msg.get<uint32_t>(), msg.get<uint32_t>(), msg.get<uint32_t>()};
	enableXTEAEncryption();
	setXTEAKey(key);
	#endif

	if (operatingSystem >= (CLIENTOS_OTCLIENT_LINUX + CLIENTOS_OTCLIENT_LINUX)) {
		disconnectClient("OTClientV8 extended features are not supported on this server.");
		return;
	}

	msg.skipBytes(1); // gamemaster flag

	#if GAME_FEATURE_SESSIONKEY > 0
	std::string sessionKey = msg.getString();

	auto sessionArgs = explodeString(sessionKey, "\n", 4);
	if (sessionArgs.size() != 4) {
		disconnect();
		return;
	}

	std::string& accountName = sessionArgs[0];
	std::string& password = sessionArgs[1];
	std::string& token = sessionArgs[2];
	uint32_t tokenTime = 0;

	try {
		tokenTime = std::stoul(sessionArgs[3]);
	} catch (const std::invalid_argument&) {
		disconnectClient("Malformed token packet.");
		return;
	} catch (const std::out_of_range&) {
		disconnectClient("Token time is too long.");
		return;
	}

	std::string characterName = msg.getString();
	#else
	#if GAME_FEATURE_ACCOUNT_NAME > 0
	std::string accountName = msg.getString();
	#else
	std::string accountName = std::to_string(msg.get<uint32_t>());
	#endif

	std::string characterName = msg.getString();
	std::string password = msg.getString();
	#if GAME_FEATURE_AUTHENTICATOR > 0
	// should we even try implementing pre sessionkey tokens?
	// it is only for 10.72 and 10.73 and we don't have timestamp
	msg.getString();
	#endif
	#endif

	if (characterName.empty() || characterName.size() > NETWORKMESSAGE_PLAYERNAME_MAXLENGTH) {
		disconnectClient("Malformed packet.");
		return;
	}

	if (accountName.empty()) {
		disconnectClient("You must enter your account name.");
		return;
	}

	if (password.empty()) {
		disconnectClient("Invalid password.");
		return;
	}

	#if GAME_FEATURE_SERVER_SENDFIRST > 0
	uint32_t timeStamp = msg.get<uint32_t>();
	uint8_t randNumber = msg.getByte();
	if (challengeTimestamp != timeStamp || challengeRandom != randNumber) {
		disconnect();
		return;
	}
	#endif

	if (g_game().getGameState() == GAME_STATE_STARTUP) {
		disconnectClient("Gameworld is starting up. Please wait.");
		return;
	}

	if (g_game().getGameState() == GAME_STATE_MAINTAIN) {
		disconnectClient("Gameworld is under maintenance. Please re-connect in a while.");
		return;
	}
	
	#if GAME_FEATURE_SESSIONKEY > 0
	g_dispatcher().addTask(std::bind(&ProtocolGame::login, getThis(), std::move(accountName), std::move(password), std::move(characterName), std::move(token), tokenTime, operatingSystem, TFCoperatingSystem));
	#else
	g_dispatcher().addTask(std::bind(&ProtocolGame::login, getThis(), std::move(accountName), std::move(password), std::move(characterName), operatingSystem, TFCoperatingSystem));
	#endif
}

void ProtocolGame::onConnect()
{
	auto output = OutputMessagePool::getOutputMessage();
	static std::random_device rd;
	static std::ranlux24 generator(rd());
	static std::uniform_int_distribution<uint16_t> randNumber(0x00, 0xFF);

	// Skip checksum
	output->skipBytes(sizeof(uint32_t));

	// Packet length & type
	output->add<uint16_t>(0x0006);
	output->addByte(0x1F);

	// Add timestamp & random number
	challengeTimestamp = static_cast<uint32_t>(time(nullptr));
	output->add<uint32_t>(challengeTimestamp);

	challengeRandom = randNumber(generator);
	output->addByte(challengeRandom);

	// Go back and write checksum
	output->skipBytes(-12);
	output->add<uint32_t>(adlerChecksum(output->getOutputBuffer() + sizeof(uint32_t), 8));

	send(output);
}

void ProtocolGame::disconnectClient(const std::string& message) const
{
	auto output = OutputMessagePool::getOutputMessage();
	output->addByte(0x14);
	output->addString(message);
	send(output);
	disconnect();
}

void ProtocolGame::writeToOutputBuffer(const NetworkMessage& msg)
{
	auto out = getOutputBuffer(msg.getLength());
	out->append(msg);
}

void ProtocolGame::parsePacket(NetworkMessage& msg)
{
	if (!acceptPackets || g_game().getGameState() == GAME_STATE_SHUTDOWN || msg.getLength() <= 0) {
		return;
	}

	uint8_t recvbyte = msg.getByte();
	if (!player) {
		if (recvbyte == 0x0F) {
			disconnect();
		}
		return;
	}

	//a dead player can not performs actions
	if (player->isRemoved() || player->getHealth() <= 0) {
		if (recvbyte == 0x0F) {
			disconnect();
			return;
		}

		if (recvbyte != 0x14) {
			return;
		}
	}

	//modules system
	if (g_modules().eventOnRecvByte(player, recvbyte, msg)) {
		if (msg.isOverrun()) {
			disconnect();
		}
		return;
	}

	switch (recvbyte) {
		case 0x14: logout(true, false); break;
		case 0x1D: g_game().playerReceivePingBack(player); break;
		case 0x1E: g_game().playerReceivePing(player); break;
		case 0x32: parseExtendedOpcode(msg); break; //otclient extended opcode
		case 0x64: parseAutoWalk(msg); break;
		case 0x65: g_game().playerMove(player, DIRECTION_NORTH); break;
		case 0x66: g_game().playerMove(player, DIRECTION_EAST); break;
		case 0x67: g_game().playerMove(player, DIRECTION_SOUTH); break;
		case 0x68: g_game().playerMove(player, DIRECTION_WEST); break;
		case 0x69: g_game().playerStopAutoWalk(player); break;
		case 0x6A: g_game().playerMove(player, DIRECTION_NORTHEAST); break;
		case 0x6B: g_game().playerMove(player, DIRECTION_SOUTHEAST); break;
		case 0x6C: g_game().playerMove(player, DIRECTION_SOUTHWEST); break;
		case 0x6D: g_game().playerMove(player, DIRECTION_NORTHWEST); break;
		case 0x6F: g_game().playerTurn(player, DIRECTION_NORTH); break;
		case 0x70: g_game().playerTurn(player, DIRECTION_EAST); break;
		case 0x71: g_game().playerTurn(player, DIRECTION_SOUTH); break;
		case 0x72: g_game().playerTurn(player, DIRECTION_WEST); break;
		case 0x73: parseTeleport(msg); break;
		case 0x77: parseEquipObject(msg); break;
		case 0x78: parseThrow(msg); break;
		case 0x79: parseLookInShop(msg); break;
		case 0x7A: parsePlayerPurchase(msg); break;
		case 0x7B: parsePlayerSale(msg); break;
		case 0x7C: g_game().playerCloseShop(player); break;
		case 0x7D: parseRequestTrade(msg); break;
		case 0x7E: parseLookInTrade(msg); break;
		case 0x7F: g_game().playerAcceptTrade(player); break;
		case 0x80: g_game().playerCloseTrade(player); break;
		case 0x82: parseUseItem(msg); break;
		case 0x83: parseUseItemEx(msg); break;
		case 0x84: parseUseWithCreature(msg); break;
		case 0x85: parseRotateItem(msg); break;
		case 0x87: parseCloseContainer(msg); break;
		case 0x88: parseUpArrowContainer(msg); break;
		case 0x89: parseTextWindow(msg); break;
		case 0x8A: parseHouseWindow(msg); break;
		case 0x8B: parseWrapableItem(msg); break;
		case 0x8C: parseLookAt(msg); break;
		case 0x8D: parseLookInBattleList(msg); break;
		case 0x8E: /* join aggression */ break;
		case 0x96: parseSay(msg); break;
		case 0x97: g_game().playerRequestChannels(player); break;
		case 0x98: parseOpenChannel(msg); break;
		case 0x99: parseCloseChannel(msg); break;
		case 0x9A: parseOpenPrivateChannel(msg); break;
		case 0x9E: g_game().playerCloseNpcChannel(player); break;
		case 0xA0: parseFightModes(msg); break;
		case 0xA1: parseAttack(msg); break;
		case 0xA2: parseFollow(msg); break;
		case 0xA3: parseInviteToParty(msg); break;
		case 0xA4: parseJoinParty(msg); break;
		case 0xA5: parseRevokePartyInvite(msg); break;
		case 0xA6: parsePassPartyLeadership(msg); break;
		case 0xA7: g_game().playerLeaveParty(player); break;
		case 0xA8: parseEnableSharedPartyExperience(msg); break;
		case 0xAA: g_game().playerCreatePrivateChannel(player); break;
		case 0xAB: parseChannelInvite(msg); break;
		case 0xAC: parseChannelExclude(msg); break;
		case 0xAD: parseCyclopediaHouseAction(msg); break;
		case 0xBE: g_game().playerCancelAttackAndFollow(player->getID()); break;
		case 0xC7: parseTournamentLeaderboard(msg); break;
		case 0xC9: /* update tile */ break;
		case 0xCA: parseUpdateContainer(msg); break;
		#if GAME_FEATURE_BROWSEFIELD > 0
		case 0xCB: parseBrowseField(msg); break;
		#endif
		#if GAME_FEATURE_CONTAINER_PAGINATION > 0
		case 0xCC: parseSeekInContainer(msg); break;
		#endif
		#if GAME_FEATURE_INSPECTION > 0
		case 0xCD: parseInspectionObject(msg); break;
		#endif
		#if GAME_FEATURE_QUEST_TRACKER > 0
		case 0xD0: parseTrackedQuestFlags(msg); break;
		#endif
		case 0xD2: g_game().playerRequestOutfit(player); break;
		case 0xD3: parseSetOutfit(msg); break;
		#if GAME_FEATURE_MOUNTS > 0
		case 0xD4: parseToggleMount(msg); break;
		#endif
		case 0xDC: parseAddVip(msg); break;
		case 0xDD: parseRemoveVip(msg); break;
		case 0xDE: parseEditVip(msg); break;
		case 0xE1: g_game().playerMonsterCyclopedia(player); break;
		case 0xE2: parseCyclopediaMonsters(msg); break;
		case 0xE3: parseCyclopediaRace(msg); break;
		case 0xE5: parseCyclopediaCharacterInfo(msg); break;
		case 0xE6: parseBugReport(msg); break;
		case 0xE7: /* thank you */ break;
		case 0xE8: parseDebugAssert(msg); break;
		case 0xF0: g_game().playerShowQuestLog(player); break;
		case 0xF1: parseQuestLine(msg); break;
		case 0xF2: parseRuleViolationReport(msg); break;
		case 0xF3: /* get object info */ break;
		#if GAME_FEATURE_MARKET > 0
		case 0xF4: parseMarketLeave(); break;
		case 0xF5: parseMarketBrowse(msg); break;
		case 0xF6: parseMarketCreateOffer(msg); break;
		case 0xF7: parseMarketCancelOffer(msg); break;
		case 0xF8: parseMarketAcceptOffer(msg); break;
		#endif
		case 0xF9: parseModalWindowAnswer(msg); break;

		default:
			// std::cout << "Player: " << player->getName() << " sent an unknown packet header: 0x" << std::hex << static_cast<uint16_t>(recvbyte) << std::dec << "!" << std::endl;
			break;
	}

	if (msg.isOverrun()) {
		disconnect();
	}
}

void ProtocolGame::GetTileDescription(const Tile* tile)
{
	int32_t count;
	Item* ground = tile->getGround();
	if (ground) {
		AddItem(ground);
		count = 1;
	} else {
		count = 0;
	}

	const TileItemVector* items = tile->getItemList();
	if (items) {
		for (auto it = items->getBeginTopItem(), end = items->getEndTopItem(); it != end; ++it) {
			AddItem(*it);
			if (++count == 10) {
				break;
			}
		}
	}

	const CreatureVector* creatures = tile->getCreatures();
	if (creatures) {
		bool playerAdded = false;
		if (count < 10) {
			for (auto it = creatures->rbegin(), end = creatures->rend(); it != end; ++it) {
				const Creature* creature = (*it);
				if (!player->canSeeCreature(creature)) {
					continue;
				}

				if (creature->getID() == player->getID()) {
					playerAdded = true;
				}

				bool known;
				uint32_t removedKnown;
				checkCreatureAsKnown(creature->getID(), known, removedKnown);
				AddCreature(creature, known, removedKnown);
				if (++count == 10) {
					break;
				}
			}
		}
		if (!playerAdded && tile->getPosition() == player->getPosition()) {
			const Creature* creature = player;

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown(creature->getID(), known, removedKnown);
			AddCreature(creature, known, removedKnown);
		}
	}

	if (items && count < 10) {
		for (auto it = ItemVector::const_reverse_iterator(items->getEndDownItem()), end = ItemVector::const_reverse_iterator(items->getBeginDownItem()); it != end; ++it) {
			AddItem(*it);
			if (++count == 10) {
				return;
			}
		}
	}
}

void ProtocolGame::GetMapDescription(int32_t x, int32_t y, int32_t z, int32_t width, int32_t height)
{
	int32_t skip = -1;
	int32_t startz, endz, zstep;
	if (z > 7) {
		startz = z - 2;
		endz = std::min<int32_t>(MAP_MAX_LAYERS - 1, z + 2);
		zstep = 1;
	} else {
		startz = 7;
		endz = 0;
		zstep = -1;
	}

	for (int32_t nz = startz; nz != endz + zstep; nz += zstep) {
		GetFloorDescription(x, y, nz, width, height, z - nz, skip);
	}

	if (skip >= 0) {
		playermsg.addByte(skip);
		playermsg.addByte(0xFF);
	}
}

void ProtocolGame::GetFloorDescription(int32_t x, int32_t y, int32_t z, int32_t width, int32_t height, int32_t offset, int32_t& skip)
{
	std::vector<Tile*> tileVector = g_game().map.getFloorTiles(x + offset, y + offset, width, height, z);
	for (Tile* tile : tileVector) {
		if (tile) {
			if (skip >= 0) {
				playermsg.addByte(skip);
				playermsg.addByte(0xFF);
			}

			skip = 0;
			GetTileDescription(tile);
		} else if (skip == 0xFE) {
			playermsg.addByte(0xFF);
			playermsg.addByte(0xFF);
			skip = -1;
		} else {
			++skip;
		}
	}
}

void ProtocolGame::checkCreatureAsKnown(uint32_t id, bool& known, uint32_t& removedKnown)
{
	auto result = knownCreatureSet.insert(id);
	if (!result.second) {
		known = true;
		return;
	}
	known = false;
	if (knownCreatureSet.size() > 1300) {
		// Look for a creature to remove
		#if GAME_FEATURE_PARTY_LIST > 0
		for (auto it = knownCreatureSet.begin(), end = knownCreatureSet.end(); it != end; ++it) {
			// We need to protect party players from removing
			Creature* creature = g_game().getCreatureByID(*it);
			if (creature && creature->getPlayer()) {
				Player* checkPlayer = creature->getPlayer();
				if (player->getParty() != checkPlayer->getParty() && !canSee(creature)) {
					removedKnown = *it;
					knownCreatureSet.erase(it);
					return;
				}
			} else if (!canSee(creature)) {
				removedKnown = *it;
				knownCreatureSet.erase(it);
				return;
			}
		}
		#else
		for (auto it = knownCreatureSet.begin(), end = knownCreatureSet.end(); it != end; ++it) {
			Creature* creature = g_game().getCreatureByID(*it);
			if (!canSee(creature)) {
				removedKnown = *it;
				knownCreatureSet.erase(it);
				return;
			}
		}
		#endif

		// Bad situation. Let's just remove anyone.
		auto it = knownCreatureSet.begin();
		if (*it == id) {
			++it;
		}

		removedKnown = *it;
		knownCreatureSet.erase(it);
	} else {
		removedKnown = 0;
	}
}

bool ProtocolGame::canSee(const Creature* c) const
{
	if (!c || !player || c->isRemoved()) {
		return false;
	}

	if (!player->canSeeCreature(c)) {
		return false;
	}

	return canSee(c->getPosition());
}

bool ProtocolGame::canSee(const Position& pos) const
{
	return canSee(pos.x, pos.y, pos.z);
}

bool ProtocolGame::canSee(int32_t x, int32_t y, int32_t z) const
{
	if (!player) {
		return false;
	}

	const Position& myPos = player->getPosition();
	if (myPos.z <= 7) {
		//we are on ground level or above (7 -> 0)
		//view is from 7 -> 0
		if (z > 7) {
			return false;
		}
	} else if (myPos.z >= 8) {
		//we are underground (8 -> 15)
		//view is +/- 2 from the floor we stand on
		if (std::abs(myPos.getZ() - z) > 2) {
			return false;
		}
	}

	//negative offset means that the action taken place is on a lower floor than ourself
	int32_t offsetz = myPos.getZ() - z;
	if ((x >= myPos.getX() - (CLIENT_MAP_WIDTH_OFFSET - 1) + offsetz) && (x <= myPos.getX() + CLIENT_MAP_WIDTH_OFFSET + offsetz) &&
	        (y >= myPos.getY() - (CLIENT_MAP_HEIGHT_OFFFSET - 1) + offsetz) && (y <= myPos.getY() + CLIENT_MAP_HEIGHT_OFFFSET + offsetz)) {
		return true;
	}
	return false;
}

// Parse methods
void ProtocolGame::parseChannelInvite(NetworkMessage& msg)
{
	const std::string name = msg.getString();
	if (!name.empty() && name.length() <= NETWORKMESSAGE_PLAYERNAME_MAXLENGTH) {
		g_game().playerChannelInvite(player, name);
	}
}

void ProtocolGame::parseChannelExclude(NetworkMessage& msg)
{
	const std::string name = msg.getString();
	if (!name.empty() && name.length() <= NETWORKMESSAGE_PLAYERNAME_MAXLENGTH) {
		g_game().playerChannelExclude(player, name);
	}
}

void ProtocolGame::parseOpenChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.get<uint16_t>();
	g_game().playerOpenChannel(player, channelId);
}

void ProtocolGame::parseCloseChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.get<uint16_t>();
	g_game().playerCloseChannel(player, channelId);
}

void ProtocolGame::parseOpenPrivateChannel(NetworkMessage& msg)
{
	std::string receiver = msg.getString();
	if (!receiver.empty() && receiver.length() <= NETWORKMESSAGE_PLAYERNAME_MAXLENGTH) {
		g_game().playerOpenPrivateChannel(player, receiver);
	}
}

#if GAME_FEATURE_QUEST_TRACKER > 0
void ProtocolGame::parseTrackedQuestFlags(NetworkMessage& msg)
{
	std::vector<uint16_t> quests;
	uint8_t missions = msg.getByte();
	for (uint8_t i = 0; i < missions; ++i) {
		quests.emplace_back(msg.get<uint16_t>());
	}
	g_game().playerResetTrackedQuests(player, quests);
}
#endif

void ProtocolGame::parseAutoWalk(NetworkMessage& msg)
{
	uint8_t numdirs = msg.getByte();
	if (numdirs == 0) {
		return;
	}

	std::vector<Direction> path;
	path.resize(numdirs, DIRECTION_NORTH);
	for (uint8_t i = 0; i < numdirs; ++i) {
		uint8_t rawdir = msg.getByte();
		switch (rawdir) {
			case 1: path[numdirs - i - 1] = DIRECTION_EAST; break;
			case 2: path[numdirs - i - 1] = DIRECTION_NORTHEAST; break;
			case 3: path[numdirs - i - 1] = DIRECTION_NORTH; break;
			case 4: path[numdirs - i - 1] = DIRECTION_NORTHWEST; break;
			case 5: path[numdirs - i - 1] = DIRECTION_WEST; break;
			case 6: path[numdirs - i - 1] = DIRECTION_SOUTHWEST; break;
			case 7: path[numdirs - i - 1] = DIRECTION_SOUTH; break;
			case 8: path[numdirs - i - 1] = DIRECTION_SOUTHEAST; break;
			default: break;
		}
	}
	g_game().playerAutoWalk(player->getID(), path);
}

void ProtocolGame::parseSetOutfit(NetworkMessage& msg)
{
	// TODO: implement outfit type
	// uint8_t outfitType = 0;
	// outfitType = msg.getByte();

	Outfit_t newOutfit;
	#if GAME_FEATURE_LOOKTYPE_U16 > 0
	newOutfit.lookType = msg.get<uint16_t>();
	#else
	newOutfit.lookType = msg.getByte();
	#endif
	newOutfit.lookHead = msg.getByte();
	newOutfit.lookBody = msg.getByte();
	newOutfit.lookLegs = msg.getByte();
	newOutfit.lookFeet = msg.getByte();
	newOutfit.lookAddons = msg.getByte();

	#if GAME_FEATURE_MOUNTS > 0
	newOutfit.lookMount = msg.get<uint16_t>();
	#endif

	g_game().playerChangeOutfit(player, newOutfit);
}

#if GAME_FEATURE_MOUNTS > 0
void ProtocolGame::parseToggleMount(NetworkMessage& msg)
{
	bool mount = msg.getByte() != 0;
	g_game().playerToggleMount(player, mount);
}
#endif

void ProtocolGame::parseUseItem(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	uint8_t index = msg.getByte();
	g_game().playerUseItem(player->getID(), pos, stackpos, index, spriteId);
}

void ProtocolGame::parseUseItemEx(NetworkMessage& msg)
{
	Position fromPos = msg.getPosition();
	uint16_t fromSpriteId = msg.get<uint16_t>();
	uint8_t fromStackPos = msg.getByte();
	Position toPos = msg.getPosition();
	uint16_t toSpriteId = msg.get<uint16_t>();
	uint8_t toStackPos = msg.getByte();
	g_game().playerUseItemEx(player->getID(), fromPos, fromStackPos, fromSpriteId, toPos, toStackPos, toSpriteId);
}

void ProtocolGame::parseUseWithCreature(NetworkMessage& msg)
{
	Position fromPos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t fromStackPos = msg.getByte();
	uint32_t creatureId = msg.get<uint32_t>();
	g_game().playerUseWithCreature(player->getID(), fromPos, fromStackPos, creatureId, spriteId);
}

void ProtocolGame::parseCloseContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.getByte();
	g_game().playerCloseContainer(player, cid);
}

void ProtocolGame::parseUpArrowContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.getByte();
	g_game().playerMoveUpContainer(player, cid);
}

void ProtocolGame::parseUpdateContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.getByte();
	g_game().playerUpdateContainer(player, cid);
}

void ProtocolGame::parseThrow(NetworkMessage& msg)
{
	Position fromPos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t fromStackpos = msg.getByte();
	Position toPos = msg.getPosition();
	uint8_t count = msg.getByte();
	if (toPos != fromPos) {
		g_game().playerMoveThing(player->getID(), fromPos, spriteId, fromStackpos, toPos, count);
	}
}

void ProtocolGame::parseWrapableItem(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	g_game().playerWrapableItem(player->getID(), pos, stackpos, spriteId);
}

void ProtocolGame::parseLookAt(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	msg.skipBytes(2); // spriteId
	uint8_t stackpos = msg.getByte();
	g_game().playerLookAt(player, pos, stackpos);
}

void ProtocolGame::parseLookInBattleList(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	g_game().playerLookInBattleList(player, creatureId);
}

void ProtocolGame::parseSay(NetworkMessage& msg)
{
	std::string receiver;
	uint16_t channelId;

	SpeakClasses type = translateSpeakClassFromClient(msg.getByte());
	if (type == TALKTYPE_NONE) {
		return;
	}
	switch (type) {
		case TALKTYPE_PRIVATE_TO:
		case TALKTYPE_PRIVATE_RED_TO:
			receiver = msg.getString();
			channelId = 0;
			break;

		case TALKTYPE_CHANNEL_Y:
		case TALKTYPE_CHANNEL_O:
		case TALKTYPE_CHANNEL_R1:
			channelId = msg.get<uint16_t>();
			break;

		default:
			channelId = 0;
			break;
	}

	std::string text = msg.getString();
	trimString(text);
	if (text.empty() || text.length() > 255 || receiver.length() > NETWORKMESSAGE_PLAYERNAME_MAXLENGTH) {
		return;
	}

	g_game().playerSay(player, channelId, type, receiver, text);
}

void ProtocolGame::parseFightModes(NetworkMessage& msg)
{
	uint8_t rawFightMode = msg.getByte(); // 1 - offensive, 2 - balanced, 3 - defensive
	uint8_t rawChaseMode = msg.getByte(); // 0 - stand while fightning, 1 - chase opponent
	uint8_t rawSecureMode = msg.getByte(); // 0 - can't attack unmarked, 1 - can attack unmarked
	// uint8_t rawPvpMode = msg.getByte(); // pvp mode introduced in 10.0

	fightMode_t fightMode;
	if (rawFightMode == 1) {
		fightMode = FIGHTMODE_ATTACK;
	} else if (rawFightMode == 2) {
		fightMode = FIGHTMODE_BALANCED;
	} else {
		fightMode = FIGHTMODE_DEFENSE;
	}

	g_game().playerSetFightModes(player, fightMode, (rawChaseMode != 0), (rawSecureMode != 0));
}

void ProtocolGame::parseAttack(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	g_game().playerSetAttackedCreature(player->getID(), creatureId);
}

void ProtocolGame::parseFollow(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	g_game().playerFollowCreature(player->getID(), creatureId);
}

void ProtocolGame::parseEquipObject(NetworkMessage& msg)
{
	uint16_t spriteId = msg.get<uint16_t>();
	// msg.get<uint8_t>();

	g_game().playerEquipItem(player, spriteId);
}

void ProtocolGame::parseTeleport(NetworkMessage& msg)
{
	Position position = msg.getPosition();
	g_game().playerTeleport(player, position);
}

void ProtocolGame::parseTextWindow(NetworkMessage& msg)
{
	uint32_t windowTextId = msg.get<uint32_t>();
	const std::string newText = msg.getString();
	g_game().playerWriteItem(player, windowTextId, newText);
}

void ProtocolGame::parseHouseWindow(NetworkMessage& msg)
{
	uint8_t doorId = msg.getByte();
	uint32_t id = msg.get<uint32_t>();
	const std::string text = msg.getString();
	g_game().playerUpdateHouseWindow(player, doorId, id, text);
}

void ProtocolGame::parseLookInShop(NetworkMessage& msg)
{
	uint16_t id = msg.get<uint16_t>();
	uint8_t count = msg.getByte();
	g_game().playerLookInShop(player, id, count);
}

void ProtocolGame::parsePlayerPurchase(NetworkMessage& msg)
{
	uint16_t id = msg.get<uint16_t>();
	uint8_t count = msg.getByte();
	uint8_t amount = msg.getByte();
	bool ignoreCap = (msg.getByte() != 0);
	bool inBackpacks = (msg.getByte() != 0);
	if (amount > 0 && amount <= 100) {
		g_game().playerPurchaseItem(player, id, count, amount, ignoreCap, inBackpacks);
	}
}

void ProtocolGame::parsePlayerSale(NetworkMessage& msg)
{
	uint16_t id = msg.get<uint16_t>();
	uint8_t count = msg.getByte();
	uint8_t amount = msg.getByte();
	bool ignoreEquipped = (msg.getByte() != 0);
	if (amount > 0 && amount <= 100) {
		g_game().playerSellItem(player, id, count, amount, ignoreEquipped);
	}
}

void ProtocolGame::parseRequestTrade(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	uint32_t playerId = msg.get<uint32_t>();
	g_game().playerRequestTrade(player->getID(), pos, stackpos, playerId, spriteId);
}

void ProtocolGame::parseLookInTrade(NetworkMessage& msg)
{
	bool counterOffer = (msg.getByte() == 0x01);
	uint8_t index = msg.getByte();
	g_game().playerLookInTrade(player, counterOffer, index);
}

void ProtocolGame::parseAddVip(NetworkMessage& msg)
{
	const std::string name = msg.getString();
	if (!name.empty() && name.length() <= NETWORKMESSAGE_PLAYERNAME_MAXLENGTH) {
		g_game().playerRequestAddVip(player, name);
	}
}

void ProtocolGame::parseRemoveVip(NetworkMessage& msg)
{
	uint32_t guid = msg.get<uint32_t>();
	g_game().playerRequestRemoveVip(player, guid);
}

void ProtocolGame::parseEditVip(NetworkMessage& msg)
{
	uint32_t guid = msg.get<uint32_t>();
	const std::string description = msg.getString();
	uint32_t icon = std::min<uint32_t>(10, msg.get<uint32_t>()); // 10 is max icon in 9.63
	bool notify = (msg.getByte() != 0);
	g_game().playerRequestEditVip(player, guid, description, icon, notify);
}

void ProtocolGame::parseRotateItem(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	g_game().playerRotateItem(player->getID(), pos, stackpos, spriteId);
}

void ProtocolGame::parseRuleViolationReport(NetworkMessage& msg)
{
	uint8_t reportType = msg.getByte();
	uint8_t reportReason = msg.getByte();
	const std::string targetName = msg.getString();
	const std::string comment = msg.getString();
	std::string translation;
	if (reportType == REPORT_TYPE_NAME) {
		translation = msg.getString();
	} else if (reportType == REPORT_TYPE_STATEMENT) {
		translation = msg.getString();
		msg.get<uint32_t>(); // statement id, used to get whatever player have said, we don't log that.
	}

	g_game().playerReportRuleViolation(player, targetName, reportType, reportReason, comment, translation);
}

void ProtocolGame::parseCyclopediaMonsters(NetworkMessage& msg)
{
	std::string race;
	uint8_t type = msg.getByte();
	if(type != 0)
		return;

	race = msg.getString();

	g_game().playerCyclopediaMonsters(player, race);
}

void ProtocolGame::parseCyclopediaRace(NetworkMessage& msg)
{
	uint16_t monsterId = msg.get<uint16_t>();
	g_game().playerCyclopediaRace(player, monsterId);
}

void ProtocolGame::parseCyclopediaHouseAction(NetworkMessage& msg)
{
	// Testing purposes - do not write code this way, it has race condition
	// but for testing purpose I don't need 100% thread-safety
	(void)msg;
	/*uint8_t houseActionType = msg.getByte();
	switch (houseActionType) {
		case 0: {
			std::string housePage = msg.getString();
			std::cout << "Test[0]:" << std::endl;
			std::cout << "String[1]: " << housePage << std::endl;
			if (housePage == "Rathleton") {
				NetworkMessage outMsg;
				outMsg.addByte(0xC7);
				outMsg.add<uint16_t>(0x01);
				outMsg.add<uint32_t>(0x4A4A);
				outMsg.addByte(1);
				outMsg.addByte(1);
				writeToOutputBuffer(outMsg);
			} else {
				NetworkMessage outMsg;
				outMsg.addByte(0xC7);
				outMsg.add<uint16_t>(0x01);
				outMsg.add<uint32_t>(0x4A4A);
				outMsg.addByte(1);
				outMsg.addByte(2);
				outMsg.addString("Test");
				outMsg.add<uint32_t>(0xFFFFFFFF);
				outMsg.addByte(1);
				outMsg.addByte(0);
				outMsg.addByte(0);
				writeToOutputBuffer(outMsg);
			}
			break;
		}
		case 1: {
			std::cout << "Test[1]:" << std::endl;
			std::cout << "U32[1]: " << msg.get<uint32_t>() << std::endl;
			std::cout << "U64[2]: " << msg.get<uint64_t>() << std::endl;
			break;
		}
		case 2: {
			std::cout << "Test[2]:" << std::endl;
			std::cout << "U32[1]: " << msg.get<uint32_t>() << std::endl;
			std::cout << "U32[2]: " << msg.get<uint32_t>() << std::endl;
			break;
		}
		case 3: {
			std::cout << "Test[3]:" << std::endl;
			std::cout << "U32[1]: " << msg.get<uint32_t>() << std::endl;
			std::cout << "U32[2]: " << msg.get<uint32_t>() << std::endl;
			std::cout << "String[3]: " << msg.getString() << std::endl;
			std::cout << "U64[4]: " << msg.get<uint64_t>() << std::endl;
			break;
		}
	}*/
}

void ProtocolGame::parseCyclopediaCharacterInfo(NetworkMessage& msg)
{
	CyclopediaCharacterInfoType_t characterInfoType;
	msg.get<uint32_t>();
	characterInfoType = static_cast<CyclopediaCharacterInfoType_t>(msg.getByte());

	g_game().playerCyclopediaCharacterInfo(player, characterInfoType);
}

void ProtocolGame::parseTournamentLeaderboard(NetworkMessage& msg)
{
	uint8_t ledaerboardType = msg.getByte();
	if(ledaerboardType == 0)
	{
		const std::string worldName = msg.getString();
		uint16_t currentPage = msg.get<uint16_t>();
		(void)worldName;
		(void)currentPage;
	}
	else if(ledaerboardType == 1)
	{
		const std::string worldName = msg.getString();
		const std::string characterName = msg.getString();
		(void)worldName;
		(void)characterName;
	}
	uint8_t elementsPerPage = msg.getByte();
	(void)elementsPerPage;

	g_game().playerTournamentLeaderboard(player, ledaerboardType);
}

void ProtocolGame::parseBugReport(NetworkMessage& msg)
{
	uint8_t category = msg.getByte();
	std::string message = msg.getString();

	Position position;
	if (category == BUG_CATEGORY_MAP) {
		position = msg.getPosition();
	}

	g_game().playerReportBug(player, message, position, category);
}

void ProtocolGame::parseDebugAssert(NetworkMessage& msg)
{
	if (debugAssertSent) {
		return;
	}

	debugAssertSent = true;

	std::string assertLine = msg.getString();
	std::string date = msg.getString();
	std::string description = msg.getString();
	std::string comment = msg.getString();
	g_game().playerDebugAssert(player, assertLine, date, description, comment);
}

void ProtocolGame::parseInviteToParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	g_game().playerInviteToParty(player, targetId);
}

void ProtocolGame::parseJoinParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	g_game().playerJoinParty(player, targetId);
}

void ProtocolGame::parseRevokePartyInvite(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	g_game().playerRevokePartyInvitation(player, targetId);
}

void ProtocolGame::parsePassPartyLeadership(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	g_game().playerPassPartyLeadership(player, targetId);
}

void ProtocolGame::parseEnableSharedPartyExperience(NetworkMessage& msg)
{
	bool sharedExpActive = (msg.getByte() == 1);
	g_game().playerEnableSharedPartyExperience(player, sharedExpActive);
}

void ProtocolGame::parseQuestLine(NetworkMessage& msg)
{
	uint16_t questId = msg.get<uint16_t>();
	g_game().playerShowQuestLine(player, questId);
}

#if GAME_FEATURE_MARKET > 0
void ProtocolGame::parseMarketLeave()
{
	g_game().playerLeaveMarket(player);
}

void ProtocolGame::parseMarketBrowse(NetworkMessage& msg)
{
	uint16_t browseId = msg.get<uint16_t>();
	if (browseId == MARKETREQUEST_OWN_OFFERS) {
		g_game().playerBrowseMarketOwnOffers(player);
	} else if (browseId == MARKETREQUEST_OWN_HISTORY) {
		g_game().playerBrowseMarketOwnHistory(player);
	} else {
		g_game().playerBrowseMarket(player, browseId);
	}
}

void ProtocolGame::parseMarketCreateOffer(NetworkMessage& msg)
{
	uint8_t type = msg.getByte();
	uint16_t spriteId = msg.get<uint16_t>();
	uint16_t amount = msg.get<uint16_t>();
	uint32_t price = msg.get<uint32_t>();
	bool anonymous = (msg.getByte() != 0);
	if (amount > 0 && amount <= 64000 && price > 0 && price <= 999999999 && (type == MARKETACTION_BUY || type == MARKETACTION_SELL)) {
		g_game().playerCreateMarketOffer(player, type, spriteId, amount, price, anonymous);
	}
}

void ProtocolGame::parseMarketCancelOffer(NetworkMessage& msg)
{
	uint32_t timestamp = msg.get<uint32_t>();
	uint16_t counter = msg.get<uint16_t>();
	g_game().playerCancelMarketOffer(player, timestamp, counter);
}

void ProtocolGame::parseMarketAcceptOffer(NetworkMessage& msg)
{
	uint32_t timestamp = msg.get<uint32_t>();
	uint16_t counter = msg.get<uint16_t>();
	uint16_t amount = msg.get<uint16_t>();
	if (amount > 0 && amount <= 64000) {
		g_game().playerAcceptMarketOffer(player, timestamp, counter, amount);
	}
}
#endif

void ProtocolGame::parseModalWindowAnswer(NetworkMessage& msg)
{
	uint32_t id = msg.get<uint32_t>();
	uint8_t button = msg.getByte();
	uint8_t choice = msg.getByte();
	g_game().playerAnswerModalWindow(player, id, button, choice);
}

#if GAME_FEATURE_BROWSEFIELD > 0
void ProtocolGame::parseBrowseField(NetworkMessage& msg)
{
	const Position& pos = msg.getPosition();
	g_game().playerBrowseField(player->getID(), pos);
}
#endif

#if GAME_FEATURE_CONTAINER_PAGINATION > 0
void ProtocolGame::parseSeekInContainer(NetworkMessage& msg)
{
	uint8_t containerId = msg.getByte();
	uint16_t index = msg.get<uint16_t>();
	g_game().playerSeekInContainer(player, containerId, index);
}
#endif

#if GAME_FEATURE_INSPECTION > 0
void ProtocolGame::parseInspectionObject(NetworkMessage& msg)
{
	uint8_t inspectionType = msg.getByte();
	if(inspectionType == INSPECT_NORMALOBJECT)
	{
		Position pos = msg.getPosition();
		g_game().playerInspectItem(player, pos);
	}
	else if(inspectionType == INSPECT_NPCTRADE || inspectionType == INSPECT_CYCLOPEDIA)
	{
		uint16_t itemId = msg.get<uint16_t>();
		uint16_t itemCount = msg.getByte();
		g_game().playerInspectItem(player, itemId, itemCount, (inspectionType == INSPECT_CYCLOPEDIA));
	}
}
#endif

// Send methods
#if GAME_FEATURE_INSPECTION > 0
void ProtocolGame::sendItemInspection(uint16_t itemId, uint8_t itemCount, const Item* item, bool cyclopedia)
{
	playermsg.reset();
	playermsg.addByte(0x76);
	playermsg.addByte(0x00);//item
	playermsg.addByte(cyclopedia ? 0x01 : 0x00);
	playermsg.addByte(0x01);

	const ItemType& it = Item::items.getItemIdByClientId(itemId);

	if (item) {
		playermsg.addString(item->getName());
		AddItem(item);
	} else {
		playermsg.addString(it.name);
		AddItem(it.id, itemCount);
	}
	playermsg.addByte(0); // imbuements

	auto descriptions = Item::getDescriptions(it, item);
	playermsg.addByte(descriptions.size());
	for (const auto& description : descriptions) {
		playermsg.addString(description.first);
		playermsg.addString(description.second);
	}
	writeToOutputBuffer(playermsg);
}
#endif

void ProtocolGame::sendOpenPrivateChannel(const std::string& receiver)
{
	playermsg.reset();
	playermsg.addByte(0xAD);
	playermsg.addString(receiver);
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_CHAT_PLAYERLIST > 0
void ProtocolGame::sendChannelEvent(uint16_t channelId, const std::string& playerName, ChannelEvent_t channelEvent)
{
	playermsg.reset();
	playermsg.addByte(0xF3);
	playermsg.add<uint16_t>(channelId);
	playermsg.addString(playerName);
	playermsg.addByte(channelEvent);
	writeToOutputBuffer(playermsg);
}
#endif

void ProtocolGame::sendCreatureOutfit(const Creature* creature, const Outfit_t& outfit)
{
	if (!canSee(creature)) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x8E);
	playermsg.add<uint32_t>(creature->getID());
	AddOutfit(outfit);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCreatureLight(const Creature* creature)
{
	if (!canSee(creature)) {
		return;
	}

	playermsg.reset();
	AddCreatureLight(creature);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendWorldLight(LightInfo lightInfo)
{
	playermsg.reset();
	AddWorldLight(lightInfo);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendTibiaTime(int32_t time)
{
	playermsg.reset();
	playermsg.addByte(0xEF);
	playermsg.addByte(time / 60);
	playermsg.addByte(time % 60);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::updateCreatureData(const Creature* creature)
{
	uint32_t cid = creature->getID();
	if (knownCreatureSet.find(cid) == knownCreatureSet.end()) {
		return;
	}

	OperatingSystem_t regularOS = player->getOperatingSystem();
	OperatingSystem_t tfcOS = player->getTfcOperatingSystem();
	if ((regularOS >= CLIENTOS_NEW_LINUX && regularOS < CLIENTOS_OTCLIENT_LINUX) || tfcOS >= CLIENTOS_TFC_ANDROID) {
		//Using some hack so that I'm don't need to modify AddCreature function
		playermsg.reset();
		playermsg.setBufferPosition(NetworkMessage::INITIAL_BUFFER_POSITION - 1);
		AddCreature(creature, false, cid);
		playermsg.setBufferPosition(NetworkMessage::INITIAL_BUFFER_POSITION);
		playermsg.addByte(0x03);
		playermsg.setLength(playermsg.getLength() - 2);
		writeToOutputBuffer(playermsg);
	} else {
		if (canSee(creature)) {
			int32_t stackpos = creature->getTile()->getStackposOfCreature(player, creature);
			if (stackpos != -1) {
				playermsg.reset();
				playermsg.addByte(0x6B);
				playermsg.addPosition(creature->getPosition());
				playermsg.addByte(stackpos);
				AddCreature(creature, false, cid);
				writeToOutputBuffer(playermsg);
				return;
			}
		}

		//Not the best choice we have here but let's update our creature
		const Position& pos = player->getPosition();
		playermsg.reset();
		playermsg.addByte(0x6A);
		playermsg.addPosition(pos);
		#if GAME_FEATURE_TILE_ADDTHING_STACKPOS > 0
		playermsg.addByte(0xFF);
		#endif
		AddCreature(creature, false, cid);
		playermsg.addByte(0x69);
		playermsg.addPosition(pos);
		Tile* tile = player->getTile();
		if (tile) {
			GetTileDescription(tile);
			playermsg.addByte(0x00);
			playermsg.addByte(0xFF);
		} else {
			playermsg.addByte(0x01);
			playermsg.addByte(0xFF);
		}
		writeToOutputBuffer(playermsg);
	}
}

void ProtocolGame::sendCreatureWalkthrough(const Creature* creature, bool walkthrough)
{
	if (!canSee(creature)) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x92);
	playermsg.add<uint32_t>(creature->getID());
	playermsg.addByte(walkthrough ? 0x00 : 0x01);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCreatureShield(const Creature* creature)
{
	if (!canSee(creature)) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x91);
	playermsg.add<uint32_t>(creature->getID());
	playermsg.addByte(player->getPartyShield(creature->getPlayer()));
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCreatureSkull(const Creature* creature)
{
	if (g_game().getWorldType() != WORLD_TYPE_PVP) {
		return;
	}

	if (!canSee(creature)) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x90);
	playermsg.add<uint32_t>(creature->getID());
	playermsg.addByte(player->getSkullClient(creature));
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCreatureType(const Creature* creature, uint8_t creatureType)
{
	playermsg.reset();
	playermsg.addByte(0x95);
	playermsg.add<uint32_t>(creature->getID());
	if (creatureType == CREATURETYPE_SUMMON_OTHERS) {
		creatureType = CREATURETYPE_SUMMON_OWN;
	}
	playermsg.addByte(creatureType);
	if (creatureType == CREATURETYPE_SUMMON_OWN) {
		const Creature* master = creature->getMaster();
		if (master) {
			playermsg.add<uint32_t>(master->getID());
		} else {
			playermsg.add<uint32_t>(0);
		}
	}
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCreatureSquare(const Creature* creature, Color_t color)
{
	if (!canSee(creature)) {
		return;
	}

	playermsg.reset();
	#if GAME_FEATURE_CREATURE_MARK > 0
	playermsg.addByte(0x93);
	playermsg.add<uint32_t>(creature->getID());
	playermsg.addByte(0x01);
	playermsg.addByte(color);
	#else
	playermsg.addByte(0x86);
	playermsg.add<uint32_t>(creature->getID());
	playermsg.addByte(color);
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendTutorial(uint8_t tutorialId)
{
	playermsg.reset();
	playermsg.addByte(0xDC);
	playermsg.addByte(tutorialId);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendAddMarker(const Position& pos, uint8_t markType, const std::string& desc)
{
	playermsg.reset();
	playermsg.addByte(0xDD);
	playermsg.addByte(0x00);
	playermsg.addPosition(pos);
	playermsg.addByte(markType);
	playermsg.addString(desc);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendMonsterCyclopedia()
{
	playermsg.reset();
	playermsg.addByte(0xD5);

	auto races = g_monsters().getRaces();
	auto monsterRaces = g_monsters().getMonsterRaces();

	playermsg.add<uint16_t>(races.size());
	for (const auto& race : races) {
		playermsg.addString(race.first);

		auto it = monsterRaces.find(race.second);
		if (it != monsterRaces.end()) {
			playermsg.add<uint16_t>(it->second.size());
			playermsg.add<uint16_t>(it->second.size());
		} else {
			playermsg.add<uint16_t>(0);
			playermsg.add<uint16_t>(0);
		}
	}
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaMonsters(const std::string& race)
{
	playermsg.reset();
	playermsg.addByte(0xD6);

	auto races = g_monsters().getRaces();
	auto monsterRaces = g_monsters().getMonsterRaces();
	playermsg.addString(race);

	auto it = races.find(race);
	if (it != races.end()) {
		auto it2 = monsterRaces.find(it->second);
		if (it2 != monsterRaces.end()) {
			playermsg.add<uint16_t>(it2->second.size());
			for (const auto& monster : it2->second) {
				uint8_t monsterProgress = BESTIARY_PROGRESS_COMPLETED;

				playermsg.add<uint16_t>(monster.first);
				playermsg.addByte(monsterProgress);
				if (monsterProgress != BESTIARY_PROGRESS_NONE) {
					playermsg.addByte(BESTIARY_OCCURENCE_COMMON);
				}
			}
		} else {
			playermsg.add<uint16_t>(0);
		}
	} else {
		playermsg.add<uint16_t>(0);
	}
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaRace(uint16_t monsterId)
{
	playermsg.reset();
	playermsg.addByte(0xD7);

	auto monsterRaces = g_monsters().getMonsterRaces();
	for (const auto& race : monsterRaces) {
		auto it = race.second.find(monsterId);
		if (it != race.second.end()) {
			MonsterType* monsterType = g_monsters().getMonsterType(it->second);
			if (monsterType) {
				uint8_t monsterProgress = BESTIARY_PROGRESS_COMPLETED;

				playermsg.add<uint16_t>(monsterId);
				playermsg.addString(g_monsters().getRaceName(race.first));
				playermsg.addByte(monsterProgress);
				playermsg.add<uint32_t>(0); // total kills
				playermsg.add<uint16_t>(0); // kills to progress 1
				playermsg.add<uint16_t>(0); // kills to progress 2
				playermsg.add<uint16_t>(0); // kills to progress 3
				if (monsterProgress >= BESTIARY_PROGRESS_FIRST) {
					playermsg.addByte(BESTIARY_DIFFICULTY_HARMLESS);
					playermsg.addByte(BESTIARY_OCCURENCE_COMMON);

					std::vector<const std::vector<LootBlock>*> lootBlocks{ &monsterType->info.lootItems };
					uint8_t lootSize = 0;
					auto startLoot = playermsg.getBufferPosition();
					playermsg.addByte(lootSize);
					for (std::vector<const std::vector<LootBlock>*>::iterator lit = lootBlocks.begin(); lit != lootBlocks.end(); lit++) {
						const std::vector<LootBlock>* lootVector = (*lit);
						for (const auto& lootBlock : *lootVector) {
							if (!lootBlock.childLoot.empty()) {
								lootBlocks.push_back(&lootBlock.childLoot);
							} else {
								uint16_t itemId = lootBlock.id;

								const ItemType& item = Item::items[itemId];
								playermsg.add<uint16_t>(item.clientId);
								if (lootBlock.chance >= 25000) {
									playermsg.addByte(BESTIARY_RARITY_COMMON);
								} else if (lootBlock.chance >= 5000) {
									playermsg.addByte(BESTIARY_RARITY_UNCOMMON);
								} else if (lootBlock.chance >= 1000) {
									playermsg.addByte(BESTIARY_RARITY_SEMIRARE);
								} else if (lootBlock.chance >= 500) {
									playermsg.addByte(BESTIARY_RARITY_RARE);
								} else {
									playermsg.addByte(BESTIARY_RARITY_VERYRARE);
								}
								playermsg.addByte(0x00); // special event item
								if (itemId != 0) { // 0 indicate hidden item
									playermsg.addString(item.name);
									playermsg.addByte((lootBlock.countmax > 1) ? 0x01 : 0x00);
								}

								if (++lootSize == 0xFF) {
									goto EndLoot;
								}
							}
						}
					}

					EndLoot:
					auto returnTo = playermsg.getBufferPosition();
					playermsg.setBufferPosition(startLoot);
					playermsg.addByte(lootSize);
					playermsg.setLength(playermsg.getLength() - 1); // decrease one extra bytes we made
					playermsg.setBufferPosition(returnTo);
				}
				if (monsterProgress >= BESTIARY_PROGRESS_SECOND) {
					playermsg.add<uint16_t>(0); // charm points
					if (!monsterType->info.isHostile) {
						playermsg.addByte(BESTIARY_ATTACKTYPE_NONE);
					} else if (monsterType->info.targetDistance > 1) {
						playermsg.addByte(BESTIARY_ATTACKTYPE_DISTANCE);
					} else {
						playermsg.addByte(BESTIARY_ATTACKTYPE_MELEE);
					}
					if (!monsterType->info.attackSpells.empty() || !monsterType->info.defenseSpells.empty()) {
						playermsg.addByte(0x01); // casts spells
					} else {
						playermsg.addByte(0x00); // casts spells
					}
					playermsg.add<uint32_t>(static_cast<uint32_t>(monsterType->info.healthMax));
					playermsg.add<uint32_t>(static_cast<uint32_t>(monsterType->info.experience));
					playermsg.add<uint16_t>(static_cast<uint16_t>(monsterType->info.baseSpeed / 2));
					playermsg.add<uint16_t>(static_cast<uint16_t>(monsterType->info.armor));
				}
				if (monsterProgress >= BESTIARY_PROGRESS_THIRD) {
					playermsg.addByte(8); // combats

					static const CombatType_t combats[] = {COMBAT_PHYSICALDAMAGE, COMBAT_FIREDAMAGE, COMBAT_EARTHDAMAGE, COMBAT_ENERGYDAMAGE, COMBAT_ICEDAMAGE, COMBAT_HOLYDAMAGE, COMBAT_DEATHDAMAGE, COMBAT_HEALING};
					for (std::underlying_type<Cipbia_Elementals_t>::type i = CIPBIA_ELEMENTAL_PHYSICAL; i <= CIPBIA_ELEMENTAL_HEALING; i++) {
						playermsg.addByte(i);

						auto combat = combats[i];
						if (monsterType->info.damageImmunities & combat) {
							playermsg.add<int16_t>(0);
						} else {
							auto combatDmg = monsterType->info.elementMap.find(combats[i]);
							if (combatDmg != monsterType->info.elementMap.end()) {
								playermsg.add<int16_t>(100-combatDmg->second);
							} else {
								playermsg.add<int16_t>(100);
							}
						}
					}

					playermsg.add<uint16_t>(1); // locations
					playermsg.addString(""); // location - TODO
				}
				if (monsterProgress >= BESTIARY_PROGRESS_COMPLETED) {
					bool monsterHaveActiveCharm = false;
					playermsg.addByte((monsterHaveActiveCharm ? 0x01 : 0x00));
					if (monsterHaveActiveCharm) {
						playermsg.addByte(0); // ??
						playermsg.add<uint32_t>(0); // ??
					} else {
						playermsg.addByte(0); // ??
					}
				}
				writeToOutputBuffer(playermsg);
				return;
			}
		}
	}

	playermsg.add<uint16_t>(monsterId);
	playermsg.addString("Extra Dimensional");
	playermsg.addByte(BESTIARY_PROGRESS_NONE);
	playermsg.add<uint32_t>(0); // total kills
	playermsg.add<uint16_t>(0); // kills to progress 1
	playermsg.add<uint16_t>(0); // kills to progress 2
	playermsg.add<uint16_t>(0); // kills to progress 3
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaBonusEffects()
{
	playermsg.reset();
	playermsg.addByte(0xD8);
	playermsg.add<int32_t>(0); // charm points
	playermsg.addByte(0); // charms
	//playermsg.addByte(10); // charmid
	//playermsg.addString("Cripple"); // charmname
	//playermsg.addString("something"); // charmdescription
	//playermsg.addByte(0); // ??
	//playermsg.add<uint16_t>(500); // charm price
	//playermsg.addByte(0); // unlocked
	//playermsg.addByte(1); // activated
	// if activated
	//playermsg.add<uint16_t>(78); // monster id
	//playermsg.add<uint32_t>(1000); // clear price

	playermsg.addByte(0); // remaining assignable charms
	playermsg.add<uint16_t>(0); // assignable monsters
	//playermsg.add<uint16_t>(78); // monster id
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterBaseInformation()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_BASEINFORMATION);
	playermsg.addByte(0x00); // No data available
	playermsg.addString(player->getName());
	playermsg.addString(player->getVocation()->getVocName());
	playermsg.add<uint16_t>(player->getLevel());
	AddOutfit(player->getDefaultOutfit());

	playermsg.addByte(0x00); // ??
	playermsg.addByte(0x00); // enable store summary & character titles
	playermsg.addString(""); // character title
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterGeneralStats()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_GENERALSTATS);
	playermsg.addByte(0x00); // No data available
	playermsg.add<uint64_t>(player->getExperience());
	playermsg.add<uint16_t>(player->getLevel());
	playermsg.addByte(player->getLevelPercent());
	playermsg.add<uint16_t>(100); // base xp gain rate
	playermsg.add<int32_t>(0); // tournament xp factor
	playermsg.add<uint16_t>(0); // low level bonus
	playermsg.add<uint16_t>(0); // xp boost
	playermsg.add<uint16_t>(100); // stamina multiplier (100 = x1.0)
	playermsg.add<uint16_t>(0); // xpBoostRemainingTime
	playermsg.addByte(0x00); // canBuyXpBoost
	playermsg.add<uint16_t>(std::min<int32_t>(player->getHealth(), std::numeric_limits<uint16_t>::max()));
	playermsg.add<uint16_t>(std::min<int32_t>(player->getMaxHealth(), std::numeric_limits<uint16_t>::max()));
	playermsg.add<uint16_t>(std::min<int32_t>(player->getMana(), std::numeric_limits<uint16_t>::max()));
	playermsg.add<uint16_t>(std::min<int32_t>(player->getMaxMana(), std::numeric_limits<uint16_t>::max()));
	playermsg.addByte(player->getSoul());
	playermsg.add<uint16_t>(player->getStaminaMinutes());

	Condition* condition = player->getCondition(CONDITION_REGENERATION);
	playermsg.add<uint16_t>(condition ? condition->getTicks() / 1000 : 0x00);
	playermsg.add<uint16_t>(player->getOfflineTrainingTime() / 60 / 1000);
	playermsg.add<uint16_t>(player->getSpeed() / 2);
	playermsg.add<uint16_t>(player->getBaseSpeed() / 2);
	playermsg.add<uint32_t>(player->getCapacity());
	playermsg.add<uint32_t>(player->getCapacity());
	playermsg.add<uint32_t>(player->getFreeCapacity());
	playermsg.addByte(8); // Skills count
	playermsg.addByte(1); // Magic Level hardcoded skill id
	playermsg.add<uint16_t>(player->getMagicLevel());
	playermsg.add<uint16_t>(player->getBaseMagicLevel());
	playermsg.add<uint16_t>(player->getBaseMagicLevel());//loyalty bonus
	playermsg.add<uint16_t>(player->getMagicLevelPercent() * 100);
	for (uint8_t i = SKILL_FIRST; i <= SKILL_LAST; ++i) { //TODO: check if all clients have the same hardcoded skill ids
		static const uint8_t HardcodedSkillIds[] = {11, 9, 8, 10, 7, 6, 13};
		playermsg.addByte(HardcodedSkillIds[i]);
		playermsg.add<uint16_t>(std::min<int32_t>(player->getSkillLevel(i), std::numeric_limits<uint16_t>::max()));
		playermsg.add<uint16_t>(player->getBaseSkill(i));
		playermsg.add<uint16_t>(player->getBaseSkill(i));//loyalty bonus
		playermsg.add<uint16_t>(player->getSkillPercent(i) * 100);
	}
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterCombatStats()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_COMBATSTATS);
	playermsg.addByte(0x00); // No data available
	for (uint8_t i = SPECIALSKILL_FIRST; i <= SPECIALSKILL_LAST; ++i) {
		playermsg.add<uint16_t>(std::min<int32_t>(100, player->varSpecialSkills[i]));
		playermsg.add<uint16_t>(0);
	}
	uint8_t haveBlesses = 0;
	uint8_t blessings = 8;
	for (uint8_t i = 1; i < blessings; i++) {
		if (player->hasBlessing(i)) {
			haveBlesses++;
		}
	}
	playermsg.addByte(haveBlesses);
	playermsg.addByte(blessings);
	playermsg.add<uint16_t>(0); // attackValue
	playermsg.addByte(0); // damageType
	playermsg.addByte(0); // convertedDamage
	playermsg.addByte(0); // convertedType
	playermsg.add<uint16_t>(0); // armorValue
	playermsg.add<uint16_t>(0); // defenseValue
	playermsg.addByte(0); // combats
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterRecentDeaths()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_RECENTDEATHS);
	playermsg.addByte(0x00); // No data available
	playermsg.add<uint16_t>(0); // current page
	playermsg.add<uint16_t>(0); // available pages
	playermsg.add<uint16_t>(0); // deaths
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterRecentPvPKills()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_RECENTPVPKILLS);
	playermsg.addByte(0x00); // No data available
	playermsg.add<uint16_t>(0); // current page
	playermsg.add<uint16_t>(0); // available pages
	playermsg.add<uint16_t>(0); // kills
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterAchievements()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_ACHIEVEMENTS);
	playermsg.addByte(0x00); // No data available
	playermsg.add<uint16_t>(0); // total points
	playermsg.add<uint16_t>(0); // total secret achievements
	playermsg.add<uint16_t>(0); // achievements
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterItemSummary()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_ITEMSUMMARY);
	playermsg.addByte(0x00); // No data available
	playermsg.add<uint16_t>(0); // ??
	playermsg.add<uint16_t>(0); // ??
	playermsg.add<uint16_t>(0); // ??
	playermsg.add<uint16_t>(0); // ??
	playermsg.add<uint16_t>(0); // ??
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterOutfitsMounts()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_OUTFITSMOUNTS);
	playermsg.addByte(0x00); // No data available
	Outfit_t currentOutfit = player->getDefaultOutfit();

	uint16_t outfitSize = 0;
	auto startOutfits = playermsg.getBufferPosition();
	playermsg.add<uint16_t>(outfitSize);

	const auto& outfits = Outfits::getInstance().getOutfits(player->getSex());
	for (const Outfit& outfit : outfits) {
		uint8_t addons;
		if (!player->getOutfitAddons(outfit, addons)) {
			continue;
		}
		outfitSize++;

		playermsg.add<uint16_t>(outfit.lookType);
		playermsg.addString(outfit.name);
		playermsg.addByte(addons);
		playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_OUTFITTYPE_NONE);
		if (outfit.lookType == currentOutfit.lookType) {
			playermsg.add<uint32_t>(1000);
		} else {
			playermsg.add<uint32_t>(0);
		}
	}
	if (outfitSize > 0) {
		playermsg.addByte(currentOutfit.lookHead);
		playermsg.addByte(currentOutfit.lookBody);
		playermsg.addByte(currentOutfit.lookLegs);
		playermsg.addByte(currentOutfit.lookFeet);
	}

	uint16_t mountSize = 0;
	auto startMounts = playermsg.getBufferPosition();
	playermsg.add<uint16_t>(mountSize);
	for (const Mount& mount : g_game().mounts.getMounts()) {
		#if GAME_FEATURE_MOUNTS > 0
		if (player->hasMount(&mount)) {
		#else
		if (true) {
		#endif
			mountSize++;

			playermsg.add<uint16_t>(mount.clientId);
			playermsg.addString(mount.name);
			playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_OUTFITTYPE_NONE);
			playermsg.add<uint32_t>(1000);
		}
	}

	playermsg.setBufferPosition(startOutfits);
	playermsg.add<uint16_t>(outfitSize);
	playermsg.setBufferPosition(startMounts);
	playermsg.add<uint16_t>(mountSize);
	playermsg.setLength(playermsg.getLength() - 4); // decrease four extra bytes we made
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterStoreSummary()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_STORESUMMARY);
	playermsg.addByte(0x00); // No data available
	playermsg.add<uint32_t>(0); // ??
	playermsg.add<uint32_t>(0); // ??
	playermsg.addByte(0x00); // ??
	playermsg.addByte(0x00); // ??
	playermsg.addByte(0x00); // ??
	playermsg.addByte(0x00); // ??
	playermsg.addByte(0x00); // ??
	playermsg.addByte(0x00); // ??
	playermsg.addByte(0x00); // ??
	playermsg.addByte(0x00); // ??
	playermsg.add<uint16_t>(0); // ??
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterInspection()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_INSPECTION);
	playermsg.addByte(0x00); // No data available
	uint8_t inventoryItems = 0;
	auto startInventory = playermsg.getBufferPosition();
	playermsg.addByte(inventoryItems);
	for (std::underlying_type<slots_t>::type slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_AMMO; slot++) {
		Item* inventoryItem = player->getInventoryItem(static_cast<slots_t>(slot));
		if (inventoryItem) {
			inventoryItems++;

			playermsg.addByte(slot);
			playermsg.addString(inventoryItem->getName());
			AddItem(inventoryItem);
			playermsg.addByte(0); // imbuements

			auto descriptions = Item::getDescriptions(Item::items[inventoryItem->getID()], inventoryItem);
			playermsg.addByte(descriptions.size());
			for (const auto& description : descriptions) {
				playermsg.addString(description.first);
				playermsg.addString(description.second);
			}
		}
	}
	playermsg.addString(player->getName());
	AddOutfit(player->getDefaultOutfit());

	playermsg.addByte(3);
	playermsg.addString("Level");
	playermsg.addString(std::to_string(player->getLevel()));
	playermsg.addString("Vocation");
	playermsg.addString(player->getVocation()->getVocName());
	playermsg.addString("Outfit");

	const Outfit* outfit = Outfits::getInstance().getOutfitByLookType(player->getSex(), player->getDefaultOutfit().lookType);
	if (outfit) {
		playermsg.addString(outfit->name);
	} else {
		playermsg.addString("unknown");
	}
	playermsg.setBufferPosition(startInventory);
	playermsg.addByte(inventoryItems);
	playermsg.setLength(playermsg.getLength() - 1); // decrease one extra byte we made
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterBadges()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_BADGES);
	playermsg.addByte(0x00); // No data available
	playermsg.addByte(0x00); // enable badges
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCyclopediaCharacterTitles()
{
	playermsg.reset();
	playermsg.addByte(0xDA);
	playermsg.addByte(CYCLOPEDIA_CHARACTERINFO_TITLES);
	playermsg.addByte(0x00); // No data available
	playermsg.addByte(0x00); // ??
	playermsg.addByte(0x00); // ??
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendTournamentLeaderboard()
{
	playermsg.reset();
	playermsg.addByte(0xC5);
	playermsg.addByte(0);
	playermsg.addByte(0x01); // No data available
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendReLoginWindow(uint8_t unfairFightReduction)
{
	playermsg.reset();
	playermsg.addByte(0x28);
	#if GAME_FEATURE_DEATH_TYPE > 0
	playermsg.addByte(0x00);
	#endif
	#if GAME_FEATURE_DEATH_PENALTY > 0
	playermsg.addByte(unfairFightReduction);
	#else
	(void)unfairFightReduction;
	#endif
	playermsg.addByte(0x01); // use death redemption (boolean)
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendStats()
{
	playermsg.reset();
	AddPlayerStats();
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendBasicData()
{
	playermsg.reset();
	playermsg.addByte(0x9F);
	if (player->isPremium()) {
		playermsg.addByte(1);
		#if GAME_FEATURE_PREMIUM_EXPIRATION > 0
		playermsg.add<uint32_t>(time(nullptr) + (player->premiumDays * 86400));
		#endif
	} else {
		playermsg.addByte(0);
		#if GAME_FEATURE_PREMIUM_EXPIRATION > 0
		playermsg.add<uint32_t>(0);
		#endif
	}
	playermsg.addByte(player->getVocation()->getClientId());

	std::vector<uint16_t> spells = g_spells().getSpellsByVocation(player->getVocationId());
	playermsg.add<uint16_t>(spells.size());
	for (auto spellId : spells) {
		playermsg.addByte(spellId);
	}
	writeToOutputBuffer(playermsg);
}

/*void ProtocolGame::sendBlessStatus()
{
	uint16_t haveBlesses = 0;
	uint8_t blessings = (8);
	for (uint8_t i = 1; i < blessings; i++) {
		if (player->hasBlessing(i)) {
			haveBlesses++;
		}
	}

	playermsg.reset();
	playermsg.addByte(0x9C);
	if (haveBlesses >= 5) {
		playermsg.add<uint16_t>((static_cast<uint16_t>(1) << haveBlesses) - 1);
	} else {
		playermsg.add<uint16_t>(0x00);
	}
	playermsg.addByte((haveBlesses >= 5 ? 2 : 1));
	writeToOutputBuffer(playermsg);
}*/

void ProtocolGame::sendTextMessage(const TextMessage& message)
{
	uint8_t messageType = translateMessageClassToClient(message.type);
	playermsg.reset();
	playermsg.addByte(0xB4);
	playermsg.addByte(messageType);
	switch (message.type) {
		case MESSAGE_DAMAGE_DEALT:
		case MESSAGE_DAMAGE_RECEIVED:
		case MESSAGE_DAMAGE_OTHERS: {
			playermsg.addPosition(message.position);
			playermsg.add<uint32_t>(message.primary.value);
			playermsg.addByte(message.primary.color);
			playermsg.add<uint32_t>(message.secondary.value);
			playermsg.addByte(message.secondary.color);
			break;
		}
		case MESSAGE_MANA:
		case MESSAGE_HEALED:
		case MESSAGE_HEALED_OTHERS:
		case MESSAGE_EXPERIENCE:
		case MESSAGE_EXPERIENCE_OTHERS: {
			playermsg.addPosition(message.position);
			playermsg.add<uint32_t>(message.primary.value);
			playermsg.addByte(message.primary.color);
			break;
		}
		case MESSAGE_GUILD:
		case MESSAGE_PARTY_MANAGEMENT:
		case MESSAGE_PARTY:
			playermsg.add<uint16_t>(message.channelId);
			break;
		default: {
			break;
		}
	}
	playermsg.addString(message.text);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendClosePrivate(uint16_t channelId)
{
	playermsg.reset();
	playermsg.addByte(0xB3);
	playermsg.add<uint16_t>(channelId);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName)
{
	playermsg.reset();
	playermsg.addByte(0xB2);
	playermsg.add<uint16_t>(channelId);
	playermsg.addString(channelName);
	#if GAME_FEATURE_CHAT_PLAYERLIST > 0
	playermsg.add<uint16_t>(0x01);
	playermsg.addString(player->getName());
	playermsg.add<uint16_t>(0x00);
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendChannelsDialog()
{
	playermsg.reset();
	playermsg.addByte(0xAB);

	const ChannelList& list = g_chat().getChannelList(*player);
	playermsg.addByte(list.size());
	for (ChatChannel* channel : list) {
		playermsg.add<uint16_t>(channel->getId());
		playermsg.addString(channel->getName());
	}

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendChannel(uint16_t channelId, const std::string& channelName, const UsersMap* channelUsers, const InvitedMap* invitedUsers)
{
	playermsg.reset();
	playermsg.addByte(0xAC);
	playermsg.add<uint16_t>(channelId);
	playermsg.addString(channelName);
	#if GAME_FEATURE_CHAT_PLAYERLIST > 0
	if (channelUsers) {
		playermsg.add<uint16_t>(channelUsers->size());
		for (const auto& it : *channelUsers) {
			playermsg.addString(it.second->getName());
		}
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (invitedUsers) {
		playermsg.add<uint16_t>(invitedUsers->size());
		for (const auto& it : *invitedUsers) {
			playermsg.addString(it.second->getName());
		}
	} else {
		playermsg.add<uint16_t>(0x00);
	}
	#else
	(void)channelUsers;
	(void)invitedUsers;
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendChannelMessage(const std::string& author, const std::string& text, SpeakClasses type, uint16_t channel)
{
	uint8_t talkType = translateSpeakClassToClient(type);
	if (talkType == TALKTYPE_NONE) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0xAA);
	#if GAME_FEATURE_MESSAGE_STATEMENT > 0
	playermsg.add<uint32_t>(0x00);
	#endif
	playermsg.addString(author);
	#if GAME_FEATURE_MESSAGE_LEVEL > 0
	playermsg.add<uint16_t>(0x00);
	#endif
	playermsg.addByte(talkType);
	playermsg.add<uint16_t>(channel);
	playermsg.addString(text);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendIcons(uint32_t icons)
{
	playermsg.reset();
	playermsg.addByte(0xA2);
	#if GAME_FEATURE_PLAYERICONS_U32 > 0
	playermsg.add<uint32_t>(icons);
	#elif GAME_FEATURE_PLAYERICONS_U16 > 0
	playermsg.add<uint16_t>(static_cast<uint16_t>(icons));
	#else
	playermsg.addByte(static_cast<uint8_t>(icons));
	#endif
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_CONTAINER_PAGINATION > 0
void ProtocolGame::sendContainer(uint8_t cid, const Container* container, bool hasParent, uint16_t firstIndex)
#else
void ProtocolGame::sendContainer(uint8_t cid, const Container* container, bool hasParent)
#endif
{
	playermsg.reset();
	playermsg.addByte(0x6E);

	playermsg.addByte(cid);
	#if GAME_FEATURE_BROWSEFIELD > 0
	if (container->getID() == ITEM_BROWSEFIELD) {
		AddItem(ITEM_BAG, 1);
		playermsg.addString("Browse Field");
	} else {
		AddItem(container);

		const std::string& containerName = container->getName();
		playermsg.addString((containerName.empty() ? (std::string("item of type ") + std::to_string(container->getID())) : containerName));
	}
	#else
	AddItem(container);
	playermsg.addString(container->getName());
	#endif

	playermsg.addByte(container->capacity());

	playermsg.addByte(hasParent ? 0x01 : 0x00);
	//can use depot search
	// playermsg.addByte(0x00);

	#if GAME_FEATURE_CONTAINER_PAGINATION > 0
	playermsg.addByte(container->isUnlocked() ? 0x01 : 0x00); // Drag and drop
	playermsg.addByte(container->hasPagination() ? 0x01 : 0x00); // Pagination

	uint32_t containerSize = container->size();
	playermsg.add<uint16_t>(containerSize);
	playermsg.add<uint16_t>(firstIndex);
	if (firstIndex < containerSize) {
		uint8_t itemsToSend = std::min<uint32_t>(std::min<uint32_t>(container->capacity(), containerSize - firstIndex), std::numeric_limits<uint8_t>::max());

		playermsg.addByte(itemsToSend);
		for (auto it = container->getItemList().begin() + firstIndex, end = it + itemsToSend; it != end; ++it) {
			AddItem(*it);
		}
	} else {
		playermsg.addByte(0x00);
	}
	#else
	uint8_t itemsToSend = std::min<uint32_t>(container->size(), std::numeric_limits<uint8_t>::max());

	playermsg.addByte(itemsToSend);
	for (auto it = container->getItemList().begin(), end = it + itemsToSend; it != end; ++it) {
		AddItem(*it);
	}
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendShop(Npc* npc, const ShopInfoList& itemList)
{
	playermsg.reset();
	playermsg.addByte(0x7A);
	#if GAME_FEATURE_NPC_NAME_ON_TRADE > 0
	playermsg.addString(npc->getName());
	#else
	(void)npc;
	#endif
	// TODO: enhance OTC to have this extra byte
	// playermsg.addItemId(ITEM_GOLD_COIN);
	// playermsg.addString(std::string());

	uint16_t itemsToSend = std::min<size_t>(itemList.size(), std::numeric_limits<uint16_t>::max());
	playermsg.add<uint16_t>(itemsToSend);

	uint16_t i = 0;
	for (auto it = itemList.begin(); i < itemsToSend; ++it, ++i) {
		AddShopItem(*it);
	}

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCloseShop()
{
	playermsg.reset();
	playermsg.addByte(0x7C);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendSaleItemList(const std::vector<ShopInfo>& shop, const std::map<uint32_t, uint32_t>& inventoryMap)
{
	//Since we already have full inventory map we shouldn't call getMoney here - it is simply wasting cpu power
	uint64_t playerMoney = 0;
	auto it = inventoryMap.find(ITEM_CRYSTAL_COIN);
	if (it != inventoryMap.end()) {
		playerMoney += static_cast<uint64_t>(it->second) * 10000;
	}
	it = inventoryMap.find(ITEM_PLATINUM_COIN);
	if (it != inventoryMap.end()) {
		playerMoney += static_cast<uint64_t>(it->second) * 100;
	}
	it = inventoryMap.find(ITEM_GOLD_COIN);
	if (it != inventoryMap.end()) {
		playerMoney += static_cast<uint64_t>(it->second);
	}

	playermsg.reset();
	playermsg.addByte(0xEE);
	playermsg.addByte(0x00);
	playermsg.add<uint64_t>(player->getBankBalance());
	writeToOutputBuffer(playermsg);

	playermsg.reset();
	playermsg.addByte(0xEE);
	playermsg.addByte(0x01);
	playermsg.add<uint64_t>(playerMoney);
	writeToOutputBuffer(playermsg);

	playermsg.reset();
	playermsg.addByte(0x7B);
	playermsg.add<uint64_t>(playerMoney);

	uint8_t itemsToSend = 0;
	auto msgPosition = playermsg.getBufferPosition();
	playermsg.skipBytes(1);

	for (const ShopInfo& shopInfo : shop) {
		if (shopInfo.sellPrice == 0) {
			continue;
		}

		uint32_t index = static_cast<uint32_t>(shopInfo.itemId);
		if (Item::items[shopInfo.itemId].isFluidContainer()) {
			index |= (static_cast<uint32_t>(shopInfo.subType) << 16);
		}

		it = inventoryMap.find(index);
		if (it != inventoryMap.end()) {
			playermsg.addItemId(shopInfo.itemId);
			playermsg.addByte(std::min<uint32_t>(it->second, std::numeric_limits<uint8_t>::max()));
			if (++itemsToSend >= 0xFF) {
				break;
			}
		}
	}

	playermsg.setBufferPosition(msgPosition);
	playermsg.addByte(itemsToSend);
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_MARKET > 0
void ProtocolGame::sendMarketEnter(uint32_t depotId)
{
	playermsg.reset();
	playermsg.addByte(0xF6);

	playermsg.add<uint64_t>(player->getBankBalance());
	playermsg.addByte(std::min<uint32_t>(IOMarket::getPlayerOfferCount(player->getGUID()), std::numeric_limits<uint8_t>::max()));

	DepotChest* depotChest = player->getDepotChest(depotId, false);
	if (!depotChest) {
		playermsg.add<uint16_t>(0x00);
		writeToOutputBuffer(playermsg);
		return;
	}

	player->setInMarket(true);

	std::map<uint16_t, uint32_t> depotItems;
	std::forward_list<Container*> containerList { depotChest, player->getInbox() };

	do {
		Container* container = containerList.front();
		containerList.pop_front();

		for (Item* item : container->getItemList()) {
			Container* c = item->getContainer();
			if (c && !c->empty()) {
				containerList.push_front(c);
				continue;
			}

			const ItemType& itemType = Item::items[item->getID()];
			if (itemType.wareId == 0) {
				continue;
			}

			if (c && (!itemType.isContainer() || c->capacity() != itemType.maxItems)) {
				continue;
			}

			if (!item->hasMarketAttributes()) {
				continue;
			}

			depotItems[itemType.wareId] += Item::countByType(item, -1);
		}
	} while (!containerList.empty());

	uint16_t itemsToSend = std::min<size_t>(depotItems.size(), std::numeric_limits<uint16_t>::max());
	playermsg.add<uint16_t>(itemsToSend);

	uint16_t i = 0;
	for (std::map<uint16_t, uint32_t>::const_iterator it = depotItems.begin(); i < itemsToSend; ++it, ++i) {
		playermsg.add<uint16_t>(it->first);
		playermsg.add<uint16_t>(std::min<uint32_t>(0xFFFF, it->second));
	}

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendMarketLeave()
{
	playermsg.reset();
	playermsg.addByte(0xF7);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendMarketBrowseItem(uint16_t itemId, const MarketOfferList& buyOffers, const MarketOfferList& sellOffers)
{
	playermsg.reset();
	playermsg.addByte(0xF9);
	playermsg.addItemId(itemId);

	playermsg.add<uint32_t>(buyOffers.size());
	for (const MarketOffer& offer : buyOffers) {
		playermsg.add<uint32_t>(offer.timestamp);
		playermsg.add<uint16_t>(offer.counter);
		playermsg.add<uint16_t>(offer.amount);
		playermsg.add<uint32_t>(offer.price);
		playermsg.addString(offer.playerName);
	}

	playermsg.add<uint32_t>(sellOffers.size());
	for (const MarketOffer& offer : sellOffers) {
		playermsg.add<uint32_t>(offer.timestamp);
		playermsg.add<uint16_t>(offer.counter);
		playermsg.add<uint16_t>(offer.amount);
		playermsg.add<uint32_t>(offer.price);
		playermsg.addString(offer.playerName);
	}

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendMarketAcceptOffer(const MarketOfferEx& offer)
{
	playermsg.reset();
	playermsg.addByte(0xF9);
	playermsg.addItemId(offer.itemId);
	if (offer.type == MARKETACTION_BUY) {
		playermsg.add<uint32_t>(0x01);
		playermsg.add<uint32_t>(offer.timestamp);
		playermsg.add<uint16_t>(offer.counter);
		playermsg.add<uint16_t>(offer.amount);
		playermsg.add<uint32_t>(offer.price);
		playermsg.addString(offer.playerName);
		playermsg.add<uint32_t>(0x00);
	} else {
		playermsg.add<uint32_t>(0x00);
		playermsg.add<uint32_t>(0x01);
		playermsg.add<uint32_t>(offer.timestamp);
		playermsg.add<uint16_t>(offer.counter);
		playermsg.add<uint16_t>(offer.amount);
		playermsg.add<uint32_t>(offer.price);
		playermsg.addString(offer.playerName);
	}

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendMarketBrowseOwnOffers(const MarketOfferList& buyOffers, const MarketOfferList& sellOffers)
{
	playermsg.reset();
	playermsg.addByte(0xF9);
	playermsg.add<uint16_t>(MARKETREQUEST_OWN_OFFERS);

	playermsg.add<uint32_t>(buyOffers.size());
	for (const MarketOffer& offer : buyOffers) {
		playermsg.add<uint32_t>(offer.timestamp);
		playermsg.add<uint16_t>(offer.counter);
		playermsg.addItemId(offer.itemId);
		playermsg.add<uint16_t>(offer.amount);
		playermsg.add<uint32_t>(offer.price);
	}

	playermsg.add<uint32_t>(sellOffers.size());
	for (const MarketOffer& offer : sellOffers) {
		playermsg.add<uint32_t>(offer.timestamp);
		playermsg.add<uint16_t>(offer.counter);
		playermsg.addItemId(offer.itemId);
		playermsg.add<uint16_t>(offer.amount);
		playermsg.add<uint32_t>(offer.price);
	}

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendMarketCancelOffer(const MarketOfferEx& offer)
{
	playermsg.reset();
	playermsg.addByte(0xF9);
	playermsg.add<uint16_t>(MARKETREQUEST_OWN_OFFERS);
	if (offer.type == MARKETACTION_BUY) {
		playermsg.add<uint32_t>(0x01);
		playermsg.add<uint32_t>(offer.timestamp);
		playermsg.add<uint16_t>(offer.counter);
		playermsg.addItemId(offer.itemId);
		playermsg.add<uint16_t>(offer.amount);
		playermsg.add<uint32_t>(offer.price);
		playermsg.add<uint32_t>(0x00);
	} else {
		playermsg.add<uint32_t>(0x00);
		playermsg.add<uint32_t>(0x01);
		playermsg.add<uint32_t>(offer.timestamp);
		playermsg.add<uint16_t>(offer.counter);
		playermsg.addItemId(offer.itemId);
		playermsg.add<uint16_t>(offer.amount);
		playermsg.add<uint32_t>(offer.price);
	}

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendMarketBrowseOwnHistory(const HistoryMarketOfferList& buyOffers, const HistoryMarketOfferList& sellOffers)
{
	uint32_t i = 0;
	std::map<uint32_t, uint16_t> counterMap;
	uint32_t buyOffersToSend = std::min<uint32_t>(buyOffers.size(), 810 + std::max<int32_t>(0, 810 - sellOffers.size()));
	uint32_t sellOffersToSend = std::min<uint32_t>(sellOffers.size(), 810 + std::max<int32_t>(0, 810 - buyOffers.size()));

	playermsg.reset();
	playermsg.addByte(0xF9);
	playermsg.add<uint16_t>(MARKETREQUEST_OWN_HISTORY);

	playermsg.add<uint32_t>(buyOffersToSend);
	for (auto it = buyOffers.begin(); i < buyOffersToSend; ++it, ++i) {
		playermsg.add<uint32_t>(it->timestamp);
		playermsg.add<uint16_t>(counterMap[it->timestamp]++);
		playermsg.addItemId(it->itemId);
		playermsg.add<uint16_t>(it->amount);
		playermsg.add<uint32_t>(it->price);
		playermsg.addByte(it->state);
	}

	counterMap.clear();
	i = 0;

	playermsg.add<uint32_t>(sellOffersToSend);
	for (auto it = sellOffers.begin(); i < sellOffersToSend; ++it, ++i) {
		playermsg.add<uint32_t>(it->timestamp);
		playermsg.add<uint16_t>(counterMap[it->timestamp]++);
		playermsg.addItemId(it->itemId);
		playermsg.add<uint16_t>(it->amount);
		playermsg.add<uint32_t>(it->price);
		playermsg.addByte(it->state);
	}

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendMarketDetail(uint16_t itemId)
{
	playermsg.reset();
	playermsg.addByte(0xF8);
	playermsg.addItemId(itemId);

	const ItemType& it = Item::items[itemId];
	if (it.armor != 0) {
		playermsg.addString(std::to_string(it.armor));
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (it.attack != 0) {
		// TODO: chance to hit, range
		// example:
		// "attack +x, chance to hit +y%, z fields"
		if (it.abilities && it.abilities->elementType != COMBAT_NONE && it.abilities->elementDamage != 0) {
			std::ostringstream ss;
			ss << it.attack << " physical +" << it.abilities->elementDamage << ' ' << getCombatName(it.abilities->elementType);
			playermsg.addString(ss.str());
		} else {
			playermsg.addString(std::to_string(it.attack));
		}
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (it.isContainer()) {
		playermsg.addString(std::to_string(it.maxItems));
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (it.defense != 0) {
		if (it.extraDefense != 0) {
			std::ostringstream ss;
			ss << it.defense << ' ' << std::showpos << it.extraDefense << std::noshowpos;
			playermsg.addString(ss.str());
		} else {
			playermsg.addString(std::to_string(it.defense));
		}
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (!it.description.empty()) {
		const std::string& descr = it.description;
		if (descr.back() == '.') {
			playermsg.addString(std::string(descr, 0, descr.length() - 1));
		} else {
			playermsg.addString(descr);
		}
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (it.decayTime != 0) {
		std::ostringstream ss;
		ss << it.decayTime << " seconds";
		playermsg.addString(ss.str());
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (it.abilities) {
		std::ostringstream ss;
		bool separator = false;
		for (size_t i = 0; i < COMBAT_COUNT; ++i) {
			if (it.abilities->absorbPercent[i] == 0) {
				continue;
			}

			if (separator) {
				ss << ", ";
			} else {
				separator = true;
			}

			ss << getCombatName(indexToCombatType(i)) << ' ' << std::showpos << it.abilities->absorbPercent[i] << std::noshowpos << '%';
		}

		playermsg.addString(ss.str());
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (it.minReqLevel != 0) {
		playermsg.addString(std::to_string(it.minReqLevel));
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (it.minReqMagicLevel != 0) {
		playermsg.addString(std::to_string(it.minReqMagicLevel));
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	playermsg.addString(it.vocationString);

	playermsg.addString(it.runeSpellName);
	if (it.abilities) {
		std::ostringstream ss;
		bool separator = false;
		for (uint8_t i = SKILL_FIRST; i <= SKILL_LAST; i++) {
			if (!it.abilities->skills[i]) {
				continue;
			}

			if (separator) {
				ss << ", ";
			} else {
				separator = true;
			}

			ss << getSkillName(i) << ' ' << std::showpos << it.abilities->skills[i] << std::noshowpos;
		}

		if (it.abilities->stats[STAT_MAGICPOINTS] != 0) {
			if (separator) {
				ss << ", ";
			} else {
				separator = true;
			}

			ss << "magic level " << std::showpos << it.abilities->stats[STAT_MAGICPOINTS] << std::noshowpos;
		}

		if (it.abilities->speed != 0) {
			if (separator) {
				ss << ", ";
			}

			ss << "speed " << std::showpos << (it.abilities->speed >> 1) << std::noshowpos;
		}

		playermsg.addString(ss.str());
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	if (it.charges != 0) {
		playermsg.addString(std::to_string(it.charges));
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	std::string weaponName = getWeaponName(it.weaponType);
	if (it.slotPosition & SLOTP_TWO_HAND) {
		if (!weaponName.empty()) {
			weaponName += ", two-handed";
		} else {
			weaponName = "two-handed";
		}
	}

	playermsg.addString(weaponName);
	if (it.weight != 0) {
		std::ostringstream ss;
		if (it.weight < 10) {
			ss << "0.0" << it.weight;
		} else if (it.weight < 100) {
			ss << "0." << it.weight;
		} else {
			std::string weightString = std::to_string(it.weight);
			weightString.insert(weightString.end() - 2, '.');
			ss << weightString;
		}
		ss << " oz";
		playermsg.addString(ss.str());
	} else {
		playermsg.add<uint16_t>(0x00);
	}

	// TODO: double check this 2bytes in OTC makertprotocol.lua
	playermsg.add<uint16_t>(0x00);

	MarketStatistics* statistics = IOMarket::getInstance().getPurchaseStatistics(itemId);
	if (statistics) {
		playermsg.addByte(0x01);
		playermsg.add<uint32_t>(statistics->numTransactions);
		playermsg.add<uint32_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), statistics->totalPrice));
		playermsg.add<uint32_t>(statistics->highestPrice);
		playermsg.add<uint32_t>(statistics->lowestPrice);
	} else {
		playermsg.addByte(0x00);
	}

	statistics = IOMarket::getInstance().getSaleStatistics(itemId);
	if (statistics) {
		playermsg.addByte(0x01);
		playermsg.add<uint32_t>(statistics->numTransactions);
		playermsg.add<uint32_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), statistics->totalPrice));
		playermsg.add<uint32_t>(statistics->highestPrice);
		playermsg.add<uint32_t>(statistics->lowestPrice);
	} else {
		playermsg.addByte(0x00);
	}

	writeToOutputBuffer(playermsg);
}
#endif

void ProtocolGame::sendQuestLog()
{
	playermsg.reset();
	playermsg.addByte(0xF0);
	playermsg.add<uint16_t>(g_game().quests.getQuestsCount(player));
	for (const Quest& quest : g_game().quests.getQuests()) {
		if (quest.isStarted(player)) {
			playermsg.add<uint16_t>(quest.getID());
			playermsg.addString(quest.getName());
			playermsg.addByte(quest.isCompleted(player));
		}
	}
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendQuestLine(const Quest* quest)
{
	playermsg.reset();
	playermsg.addByte(0xF1);
	playermsg.add<uint16_t>(quest->getID());
	playermsg.addByte(quest->getMissionsCount(player));
	for (const Mission& mission : quest->getMissions()) {
		if (mission.isStarted(player)) {
			#if GAME_FEATURE_QUEST_TRACKER > 0
			playermsg.add<uint16_t>(mission.getMissionId());
			#endif
			playermsg.addString(mission.getName(player));
			playermsg.addString(mission.getDescription(player));
		}
	}
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_QUEST_TRACKER > 0
void ProtocolGame::sendTrackedQuests(uint8_t remainingQuests, std::vector<uint16_t>& quests)
{
	playermsg.reset();
	playermsg.addByte(0xD0);
	playermsg.addByte(0x01);
	playermsg.addByte(remainingQuests);
	playermsg.addByte(static_cast<uint8_t>(quests.size()));
	for (uint16_t missionId : quests) {
		const Mission* mission = g_game().quests.getMissionByID(missionId);
		if (mission) {
			Quest* quest = g_game().quests.getQuestByID(mission->getQuestId());
			playermsg.add<uint16_t>(missionId);
			playermsg.addString((quest ? quest->getName() : std::string()));
			playermsg.addString(mission->getName(player));
			playermsg.addString(mission->getDescription(player));
		} else {
			playermsg.add<uint16_t>(missionId);
			playermsg.addString("Unknown Error");
			playermsg.addString("Unknown Error");
			playermsg.addString("Unknown Error");
		}
	}
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendUpdateTrackedQuest(const Mission* mission)
{
	playermsg.reset();
	playermsg.addByte(0xD0);
	playermsg.addByte(0x00);
	playermsg.add<uint16_t>(mission->getMissionId());
	playermsg.addString(mission->getName(player));
	playermsg.addString(mission->getDescription(player));
	writeToOutputBuffer(playermsg);
}
#endif

void ProtocolGame::sendTradeItemRequest(const std::string& traderName, const Item* item, bool ack)
{
	playermsg.reset();
	if (ack) {
		playermsg.addByte(0x7D);
	} else {
		playermsg.addByte(0x7E);
	}

	playermsg.addString(traderName);
	if (const Container* tradeContainer = item->getContainer()) {
		std::list<const Container*> listContainer {tradeContainer};
		std::list<const Item*> itemList {tradeContainer};
		while (!listContainer.empty()) {
			const Container* container = listContainer.front();
			listContainer.pop_front();
			for (Item* containerItem : container->getItemList()) {
				Container* tmpContainer = containerItem->getContainer();
				if (tmpContainer) {
					listContainer.push_back(tmpContainer);
				}
				itemList.push_back(containerItem);
			}
		}

		playermsg.addByte(itemList.size());
		for (const Item* listItem : itemList) {
			AddItem(listItem);
		}
	} else {
		playermsg.addByte(0x01);
		AddItem(item);
	}
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCloseTrade()
{
	playermsg.reset();
	playermsg.addByte(0x7F);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCloseContainer(uint8_t cid)
{
	playermsg.reset();
	playermsg.addByte(0x6F);
	playermsg.addByte(cid);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCreatureTurn(const Creature* creature, uint32_t stackPos)
{
	if (!canSee(creature)) {
		return;
	}
	
	playermsg.reset();
	playermsg.addByte(0x6B);
	playermsg.addPosition(creature->getPosition());
	playermsg.addByte(stackPos);
	playermsg.add<uint16_t>(0x63);
	playermsg.add<uint32_t>(creature->getID());
	playermsg.addByte(creature->getDirection());
	playermsg.addByte(player->canWalkthroughEx(creature) ? 0x00 : 0x01);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCreatureSay(const Creature* creature, SpeakClasses type, const std::string& text, const Position* pos/* = nullptr*/)
{
	uint8_t talkType = translateSpeakClassToClient(type);
	if (talkType == TALKTYPE_NONE) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0xAA);
	#if GAME_FEATURE_MESSAGE_STATEMENT > 0
	static uint32_t statementId = 0;
	playermsg.add<uint32_t>(++statementId);
	#endif
	playermsg.addString(creature->getName());

	//Add level only for players
	#if GAME_FEATURE_MESSAGE_LEVEL > 0
	if (const Player* speaker = creature->getPlayer()) {
		playermsg.add<uint16_t>(speaker->getLevel());
	} else {
		playermsg.add<uint16_t>(0x00);
	}
	#endif

	playermsg.addByte(talkType);
	if (pos) {
		playermsg.addPosition(*pos);
	} else {
		playermsg.addPosition(creature->getPosition());
	}

	playermsg.addString(text);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendToChannel(const Creature* creature, SpeakClasses type, const std::string& text, uint16_t channelId)
{
	uint8_t talkType = translateSpeakClassToClient(type);
	if (talkType == TALKTYPE_NONE) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0xAA);
	#if GAME_FEATURE_MESSAGE_STATEMENT > 0
	static uint32_t statementId = 0;
	playermsg.add<uint32_t>(++statementId);
	#endif
	if (!creature) {
		playermsg.add<uint16_t>(0x00);
		#if GAME_FEATURE_MESSAGE_LEVEL > 0
		playermsg.add<uint16_t>(0x00);
		#endif
	} else if (type == TALKTYPE_CHANNEL_R2) {
		playermsg.add<uint16_t>(0x00);
		#if GAME_FEATURE_MESSAGE_LEVEL > 0
		playermsg.add<uint16_t>(0x00);
		#endif
		type = TALKTYPE_CHANNEL_R1;
	} else {
		playermsg.addString(creature->getName());

		//Add level only for players
		#if GAME_FEATURE_MESSAGE_LEVEL > 0
		if (const Player* speaker = creature->getPlayer()) {
			playermsg.add<uint16_t>(speaker->getLevel());
		} else {
			playermsg.add<uint16_t>(0x00);
		}
		#endif
	}

	playermsg.addByte(talkType);
	playermsg.add<uint16_t>(channelId);
	playermsg.addString(text);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendPrivateMessage(const Player* speaker, SpeakClasses type, const std::string& text)
{
	uint8_t talkType = translateSpeakClassToClient(type);
	if (talkType == TALKTYPE_NONE) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0xAA);
	#if GAME_FEATURE_MESSAGE_STATEMENT > 0
	static uint32_t statementId = 0;
	playermsg.add<uint32_t>(++statementId);
	#endif
	if (speaker) {
		playermsg.addString(speaker->getName());
		#if GAME_FEATURE_MESSAGE_LEVEL > 0
		playermsg.add<uint16_t>(speaker->getLevel());
		#endif
	} else {
		playermsg.add<uint16_t>(0x00);
		#if GAME_FEATURE_MESSAGE_LEVEL > 0
		playermsg.add<uint16_t>(0x00);
		#endif
	}
	playermsg.addByte(talkType);
	playermsg.addString(text);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCancelTarget()
{
	playermsg.reset();
	playermsg.addByte(0xA3);
	#if GAME_FEATURE_ATTACK_SEQUENCE > 0
	playermsg.add<uint32_t>(0x00);
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendChangeSpeed(const Creature* creature, uint32_t speed)
{
	playermsg.reset();
	playermsg.addByte(0x8F);
	playermsg.add<uint32_t>(creature->getID());
	#if GAME_FEATURE_NEWSPEED_LAW > 0
	playermsg.add<uint16_t>(creature->getBaseSpeed() / 2);
	playermsg.add<uint16_t>(speed / 2);
	#else
	playermsg.add<uint16_t>(creature->getBaseSpeed());
	playermsg.add<uint16_t>(speed);
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendCancelWalk()
{
	playermsg.reset();
	playermsg.addByte(0xB5);
	playermsg.addByte(player->getDirection());
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendSkills()
{
	playermsg.reset();
	AddPlayerSkills();
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendPing()
{
	playermsg.reset();
	#if GAME_FEATURE_PING > 0
	playermsg.addByte(0x1D);
	#else
	playermsg.addByte(0x1E);
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendPingBack()
{
	playermsg.reset();
	playermsg.addByte(0x1E);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendDistanceShoot(const Position& from, const Position& to, uint8_t type)
{
	// TODO: improve OTC to parse new effect model
	// #if CLIENT_VERSION >= 1203
	// playermsg.reset();
	// playermsg.addByte(0x83);
	// playermsg.addPosition(from);
	// playermsg.addByte(MAGIC_EFFECTS_CREATE_DISTANCEEFFECT);
	// playermsg.addByte(type);
	// playermsg.addByte(static_cast<uint8_t>(static_cast<int8_t>(static_cast<int16_t>(to.x - from.x))));
	// playermsg.addByte(static_cast<uint8_t>(static_cast<int8_t>(static_cast<int16_t>(to.y - from.y))));
	// playermsg.addByte(MAGIC_EFFECTS_END_LOOP);
	// writeToOutputBuffer(playermsg);
	// #else
	playermsg.reset();
	playermsg.addByte(0x85);
	playermsg.addPosition(from);
	playermsg.addPosition(to);
	playermsg.addByte(type);
	writeToOutputBuffer(playermsg);
	// #endif
}

void ProtocolGame::sendMagicEffect(const Position& pos, uint8_t type)
{
	if (!canSee(pos)) {
		return;
	}

	// TODO: improve OTC to parse new effect model
	// #if CLIENT_VERSION >= 1203
	// playermsg.reset();
	// playermsg.addByte(0x83);
	// playermsg.addPosition(pos);
	// playermsg.addByte(MAGIC_EFFECTS_CREATE_EFFECT);
	// playermsg.addByte(type);
	// playermsg.addByte(MAGIC_EFFECTS_END_LOOP);
	// writeToOutputBuffer(playermsg);
	// #else
	playermsg.reset();
	playermsg.addByte(0x83);
	playermsg.addPosition(pos);
	playermsg.addByte(type);
	writeToOutputBuffer(playermsg);
	// #endif
}

void ProtocolGame::sendCreatureHealth(const Creature* creature, uint8_t healthPercent)
{
	playermsg.reset();
	playermsg.addByte(0x8C);
	playermsg.add<uint32_t>(creature->getID());
	playermsg.addByte(healthPercent);
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_PARTY_LIST > 0
void ProtocolGame::sendPartyCreatureUpdate(const Creature* target)
{
	bool known;
	uint32_t removedKnown;
	uint32_t cid = target->getID();
	checkCreatureAsKnown(cid, known, removedKnown);

	playermsg.reset();
	playermsg.addByte(0x8B);
	playermsg.add<uint32_t>(cid);
	playermsg.addByte(0);//creature update
	AddCreature(player, known, removedKnown);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendPartyCreatureShield(const Creature* target)
{
	uint32_t cid = target->getID();
	if (knownCreatureSet.find(cid) == knownCreatureSet.end()) {
		sendPartyCreatureUpdate(target);
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x91);
	playermsg.add<uint32_t>(target->getID());
	playermsg.addByte(player->getPartyShield(target->getPlayer()));
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendPartyCreatureSkull(const Creature* target)
{
	if (g_game().getWorldType() != WORLD_TYPE_PVP) {
		return;
	}

	uint32_t cid = target->getID();
	if (knownCreatureSet.find(cid) == knownCreatureSet.end()) {
		sendPartyCreatureUpdate(target);
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x90);
	playermsg.add<uint32_t>(target->getID());
	playermsg.addByte(player->getSkullClient(target));
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendPartyCreatureHealth(const Creature* target, uint8_t healthPercent)
{
	uint32_t cid = target->getID();
	if (knownCreatureSet.find(cid) == knownCreatureSet.end()) {
		sendPartyCreatureUpdate(target);
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x8C);
	playermsg.add<uint32_t>(cid);
	playermsg.addByte(healthPercent);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendPartyPlayerMana(const Player* target, uint8_t manaPercent)
{
	uint32_t cid = target->getID();
	if (knownCreatureSet.find(cid) == knownCreatureSet.end()) {
		sendPartyCreatureUpdate(target);
	}

	playermsg.reset();
	playermsg.addByte(0x8B);
	playermsg.add<uint32_t>(cid);
	playermsg.addByte(11);//mana percent
	playermsg.addByte(manaPercent);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendPartyCreatureShowStatus(const Creature* target, bool showStatus)
{
	uint32_t cid = target->getID();
	if (knownCreatureSet.find(cid) == knownCreatureSet.end()) {
		sendPartyCreatureUpdate(target);
	}

	playermsg.reset();
	playermsg.addByte(0x8B);
	playermsg.add<uint32_t>(cid);
	playermsg.addByte(12);//show status
	playermsg.addByte((showStatus ? 0x01 : 0x00));
	writeToOutputBuffer(playermsg);
}
#endif

void ProtocolGame::sendFYIBox(const std::string& message)
{
	playermsg.reset();
	playermsg.addByte(0x15);
	playermsg.addString(message);
	writeToOutputBuffer(playermsg);
}

//tile
void ProtocolGame::sendMapDescription(const Position& pos)
{
	playermsg.reset();
	playermsg.addByte(0x64);
	playermsg.addPosition(player->getPosition());
	GetMapDescription(pos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), pos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), pos.z, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT);
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_TILE_ADDTHING_STACKPOS > 0
void ProtocolGame::sendAddTileItem(const Position& pos, uint32_t stackpos, const Item* item)
#else
void ProtocolGame::sendAddTileItem(const Position& pos, const Item* item)
#endif
{
	if (!canSee(pos)) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x6A);
	playermsg.addPosition(pos);
	#if GAME_FEATURE_TILE_ADDTHING_STACKPOS > 0
	playermsg.addByte(stackpos);
	#endif
	AddItem(item);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendUpdateTileItem(const Position& pos, uint32_t stackpos, const Item* item)
{
	if (!canSee(pos)) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x6B);
	playermsg.addPosition(pos);
	playermsg.addByte(stackpos);
	AddItem(item);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendRemoveTileThing(const Position& pos, uint32_t stackpos)
{
	if (!canSee(pos)) {
		return;
	}

	playermsg.reset();
	RemoveTileThing(pos, stackpos);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendUpdateTile(const Tile* tile, const Position& pos)
{
	if (!canSee(pos)) {
		return;
	}

	playermsg.reset();
	playermsg.addByte(0x69);
	playermsg.addPosition(pos);
	if (tile) {
		GetTileDescription(tile);
		playermsg.addByte(0x00);
		playermsg.addByte(0xFF);
	} else {
		playermsg.addByte(0x01);
		playermsg.addByte(0xFF);
	}

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendFightModes()
{
	playermsg.reset();
	playermsg.addByte(0xA7);
	playermsg.addByte(player->fightMode);
	playermsg.addByte(player->chaseMode);
	playermsg.addByte(player->secureMode);
	playermsg.addByte(PVP_MODE_DOVE);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendAddCreature(const Creature* creature, const Position& pos, int32_t stackpos, bool isLogin)
{
	if (!canSee(pos)) {
		return;
	}

	if (creature != player) {
		if (stackpos != -1) {
			playermsg.reset();
			playermsg.addByte(0x6A);
			playermsg.addPosition(pos);
			playermsg.addByte(stackpos);

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown(creature->getID(), known, removedKnown);
			AddCreature(creature, known, removedKnown);
			writeToOutputBuffer(playermsg);
		}

		if (isLogin) {
			sendMagicEffect(pos, CONST_ME_TELEPORT);
		}
		return;
	}

	playermsg.reset();

	#if GAME_FEATURE_LOGIN_PENDING > 0
	playermsg.addByte(0x17);
	#else
	playermsg.addByte(0x0A);
	#endif

	playermsg.add<uint32_t>(player->getID());
	playermsg.add<uint16_t>(SERVER_BEAT_MILISECONDS);

	#if GAME_FEATURE_NEWSPEED_LAW > 0
	playermsg.addDouble(Creature::speedA, 3);
	playermsg.addDouble(Creature::speedB, 3);
	playermsg.addDouble(Creature::speedC, 3);
	#endif

	// can report bugs?
	if (player->getAccountType() >= ACCOUNT_TYPE_TUTOR) {
		playermsg.addByte(0x01);
	} else {
		playermsg.addByte(0x00);
	}

	playermsg.addByte(0x00); // can change pvp framing option
	playermsg.addByte(0x00); // expert mode button enabled

	#if GAME_FEATURE_STORE > 0
	playermsg.addString(g_config().getString(ConfigManager::STORE_URL)); // URL (string) to ingame store images
	playermsg.add<uint16_t>(static_cast<uint16_t>(g_config().getNumber(ConfigManager::STORE_COIN_PACKAGES))); // premium coin package size
	#endif

	if (addExivaRestrictions) {
		playermsg.addByte(0x01); // exiva button enabled
	}

	#if GAME_FEATURE_TOURNAMENTS > 0
	playermsg.addByte(0x00); // tournament button enabled
	#endif

	#if GAME_FEATURE_LOGIN_PENDING > 0
	playermsg.addByte(0x0A); // sendPendingStateEntered
	playermsg.addByte(0x0F); // sendWorldEnter
	#endif

	//gameworld settings
	AddWorldLight(g_game().getWorldLightInfo());

	writeToOutputBuffer(playermsg);

	sendTibiaTime(g_game().getLightHour());
	sendMapDescription(pos);
	if (isLogin) {
		sendMagicEffect(pos, CONST_ME_TELEPORT);
	}

	for (int i = CONST_SLOT_FIRST; i <= CONST_SLOT_LAST; ++i) {
		sendInventoryItem(static_cast<slots_t>(i), player->getInventoryItem(static_cast<slots_t>(i)));
	}

	sendStats();
	sendSkills();
	//sendBlessStatus();

	//player light level
	sendCreatureLight(creature);
	sendVIPEntries();

	sendBasicData();
	player->sendIcons();
}

void ProtocolGame::sendMoveCreature(const Creature* creature, const Position& newPos, int32_t newStackPos, const Position& oldPos, int32_t oldStackPos, bool teleport)
{
	if (creature == player) {
		if (oldStackPos >= 10) {
			sendMapDescription(newPos);
		} else if (teleport) {
			playermsg.reset();
			RemoveTileThing(oldPos, oldStackPos);
			writeToOutputBuffer(playermsg);
			sendMapDescription(newPos);
		} else {
			playermsg.reset();
			if (oldPos.z == 7 && newPos.z >= 8) {
				RemoveTileThing(oldPos, oldStackPos);
			} else {
				playermsg.addByte(0x6D);
				playermsg.addPosition(oldPos);
				playermsg.addByte(oldStackPos);
				playermsg.addPosition(newPos);
			}

			if (newPos.z > oldPos.z) {
				MoveDownCreature(creature, newPos, oldPos);
			} else if (newPos.z < oldPos.z) {
				MoveUpCreature(creature, newPos, oldPos);
			}

			if (oldPos.y > newPos.y) { // north, for old x
				playermsg.addByte(0x65);
				GetMapDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), newPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), newPos.z, CLIENT_MAP_WIDTH, 1);
			} else if (oldPos.y < newPos.y) { // south, for old x
				playermsg.addByte(0x67);
				GetMapDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), newPos.y + CLIENT_MAP_HEIGHT_OFFFSET, newPos.z, CLIENT_MAP_WIDTH, 1);
			}

			if (oldPos.x < newPos.x) { // east, [with new y]
				playermsg.addByte(0x66);
				GetMapDescription(newPos.x + CLIENT_MAP_WIDTH_OFFSET, newPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), newPos.z, 1, CLIENT_MAP_HEIGHT);
			} else if (oldPos.x > newPos.x) { // west, [with new y]
				playermsg.addByte(0x68);
				GetMapDescription(newPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), newPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), newPos.z, 1, CLIENT_MAP_HEIGHT);
			}
			writeToOutputBuffer(playermsg);
		}
	} else if (canSee(oldPos) && canSee(creature->getPosition())) {
		if (teleport || (oldPos.z == 7 && newPos.z >= 8) || oldStackPos >= 10) {
			sendRemoveTileThing(oldPos, oldStackPos);
			sendAddCreature(creature, newPos, newStackPos, false);
		} else {
			playermsg.reset();
			playermsg.addByte(0x6D);
			playermsg.addPosition(oldPos);
			playermsg.addByte(oldStackPos);
			playermsg.addPosition(creature->getPosition());
			writeToOutputBuffer(playermsg);
		}
	} else if (canSee(oldPos)) {
		sendRemoveTileThing(oldPos, oldStackPos);
	} else if (canSee(creature->getPosition())) {
		sendAddCreature(creature, newPos, newStackPos, false);
	}
}

void ProtocolGame::sendInventoryItem(slots_t slot, const Item* item)
{
	playermsg.reset();
	if (item) {
		playermsg.addByte(0x78);
		playermsg.addByte(slot);
		AddItem(item);
	} else {
		playermsg.addByte(0x79);
		playermsg.addByte(slot);
	}
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_INVENTORY_LIST > 0
void ProtocolGame::sendItems(const std::map<uint32_t, uint32_t>& inventoryMap)
{
	playermsg.reset();
	playermsg.addByte(0xF5);

	uint16_t itemsToSend = 11;
	auto msgPosition = playermsg.getBufferPosition();
	playermsg.skipBytes(2);

	for (uint16_t i = 1; i <= 11; i++) {
		playermsg.add<uint16_t>(i);
		playermsg.addByte(0);
		playermsg.add<uint16_t>(1);
	}

	for (const auto& inventoryInfo : inventoryMap) {
		uint32_t index = inventoryInfo.first;
		uint8_t fluidType = static_cast<uint8_t>(index >> 16);

		playermsg.addItemId(static_cast<uint16_t>(index));
		playermsg.addByte((fluidType ? serverFluidToClient(fluidType) : 0));
		playermsg.add<uint16_t>(std::min<uint32_t>(inventoryInfo.second, std::numeric_limits<uint16_t>::max()));

		//Limit it to upper networkmessage buffer size incase player have very large inventory
		if (++itemsToSend >= 0x32F0) {
			break;
		}
	}

	playermsg.setBufferPosition(msgPosition);
	playermsg.add<uint16_t>(itemsToSend);
	writeToOutputBuffer(playermsg);
}
#endif

#if GAME_FEATURE_CONTAINER_PAGINATION > 0
void ProtocolGame::sendAddContainerItem(uint8_t cid, uint16_t slot, const Item* item)
#else
void ProtocolGame::sendAddContainerItem(uint8_t cid, const Item* item)
#endif
{
	playermsg.reset();
	playermsg.addByte(0x70);
	playermsg.addByte(cid);
	#if GAME_FEATURE_CONTAINER_PAGINATION > 0
	playermsg.add<uint16_t>(slot);
	#endif
	AddItem(item);
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_CONTAINER_PAGINATION > 0
void ProtocolGame::sendUpdateContainerItem(uint8_t cid, uint16_t slot, const Item* item)
#else
void ProtocolGame::sendUpdateContainerItem(uint8_t cid, uint8_t slot, const Item* item)
#endif
{
	playermsg.reset();
	playermsg.addByte(0x71);
	playermsg.addByte(cid);
	#if GAME_FEATURE_CONTAINER_PAGINATION > 0
	playermsg.add<uint16_t>(slot);
	#else
	playermsg.addByte(slot);
	#endif
	AddItem(item);
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_CONTAINER_PAGINATION > 0
void ProtocolGame::sendRemoveContainerItem(uint8_t cid, uint16_t slot, const Item* lastItem)
#else
void ProtocolGame::sendRemoveContainerItem(uint8_t cid, uint8_t slot)
#endif
{
	playermsg.reset();
	playermsg.addByte(0x72);
	playermsg.addByte(cid);
	#if GAME_FEATURE_CONTAINER_PAGINATION > 0
	playermsg.add<uint16_t>(slot);
	if (lastItem) {
		AddItem(lastItem);
	} else {
		playermsg.add<uint16_t>(0x00);
	}
	#else
	playermsg.addByte(slot);
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, Item* item, uint16_t maxlen, bool canWrite)
{
	playermsg.reset();
	playermsg.addByte(0x96);
	playermsg.add<uint32_t>(windowTextId);
	AddItem(item);
	if (canWrite) {
		playermsg.add<uint16_t>(maxlen);
		playermsg.addString(item->getText());
	} else {
		const std::string& text = item->getText();
		playermsg.add<uint16_t>(text.size());
		playermsg.addString(text);
	}

	const std::string& writer = item->getWriter();
	if (!writer.empty()) {
		playermsg.addString(writer);
	} else {
		playermsg.add<uint16_t>(0x00);
	}
	
	#if GAME_FEATURE_WRITABLE_DATE > 0
	time_t writtenDate = item->getDate();
	if (writtenDate != 0) {
		playermsg.addString(formatDateShort(writtenDate));
	} else {
		playermsg.add<uint16_t>(0x00);
	}
	#endif

	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, uint32_t itemId, const std::string& text)
{
	playermsg.reset();
	playermsg.addByte(0x96);
	playermsg.add<uint32_t>(windowTextId);
	AddItem(itemId, 1);
	playermsg.add<uint16_t>(text.size());
	playermsg.addString(text);
	playermsg.add<uint16_t>(0x00);
	#if GAME_FEATURE_WRITABLE_DATE > 0
	playermsg.add<uint16_t>(0x00);
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendHouseWindow(uint32_t windowTextId, const std::string& text)
{
	playermsg.reset();
	playermsg.addByte(0x97);
	playermsg.addByte(0x00);
	playermsg.add<uint32_t>(windowTextId);
	playermsg.addString(text);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendOutfitWindow()
{
	playermsg.reset();
	playermsg.addByte(0xC8);

	Outfit_t currentOutfit = player->getDefaultOutfit();
	#if GAME_FEATURE_MOUNTS > 0
	bool mounted = false;
	Mount* currentMount = g_game().mounts.getMountByID(player->getCurrentMount());
	if (currentMount) {
		mounted = (currentOutfit.lookMount == currentMount->clientId);
		currentOutfit.lookMount = currentMount->clientId;
	}
	#endif

	AddOutfit(currentOutfit);

	std::vector<ProtocolOutfit> protocolOutfits;
	if (player->isAccessPlayer()) {
		static const std::string gmOutfitName = "Gamemaster";
		protocolOutfits.emplace_back(gmOutfitName, 75, 0);
		
		static const std::string csOutfitName = "Customer Support";
		protocolOutfits.emplace_back(csOutfitName, 266, 0);

		static const std::string cmOutfitName = "Community Manager";
		protocolOutfits.emplace_back(cmOutfitName, 302, 0);
	}

	const auto& outfits = Outfits::getInstance().getOutfits(player->getSex());
	protocolOutfits.reserve(outfits.size());
	for (const Outfit& outfit : outfits) {
		uint8_t addons;
		if (!player->getOutfitAddons(outfit, addons)) {
			continue;
		}
		protocolOutfits.emplace_back(outfit.name, outfit.lookType, addons);
	}

	playermsg.add<uint16_t>(protocolOutfits.size());
	for (const ProtocolOutfit& outfit : protocolOutfits) {
		playermsg.add<uint16_t>(outfit.lookType);
		playermsg.addString(outfit.name);
		playermsg.addByte(outfit.addons);
		playermsg.addByte(0x00);
	}

	#if GAME_FEATURE_MOUNTS > 0
	std::vector<const Mount*> mounts;
	for (const Mount& mount : g_game().mounts.getMounts()) {
		if (player->hasMount(&mount)) {
			mounts.push_back(&mount);
		}
	}
	
	playermsg.add<uint16_t>(mounts.size());
	for (const Mount* mount : mounts) {
		playermsg.add<uint16_t>(mount->clientId);
		playermsg.addString(mount->name);
		playermsg.addByte(0x00);
	}
	#endif
	
	playermsg.addByte(0x00);//Try outfit
	playermsg.addByte(mounted ? 0x01 : 0x00);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendUpdatedVIPStatus(uint32_t guid, VipStatus_t newStatus)
{
	playermsg.reset();
	#if GAME_FEATURE_VIP_STATUS > 0
	playermsg.addByte(0xD3);
	playermsg.add<uint32_t>(guid);
	playermsg.addByte(newStatus);
	#else
	playermsg.addByte(newStatus == VIPSTATUS_OFFLINE ? 0xD4 : 0xD3);
	playermsg.add<uint32_t>(guid);
	#endif
	writeToOutputBuffer(playermsg);
}

#if GAME_FEATURE_ADDITIONAL_VIPINFO > 0
void ProtocolGame::sendVIP(uint32_t guid, const std::string& name, const std::string& description, uint32_t icon, bool notify, VipStatus_t status)
#else
void ProtocolGame::sendVIP(uint32_t guid, const std::string& name, VipStatus_t status)
#endif
{
	playermsg.reset();
	playermsg.addByte(0xD2);
	playermsg.add<uint32_t>(guid);
	playermsg.addString(name);
	#if GAME_FEATURE_ADDITIONAL_VIPINFO > 0
	playermsg.addString(description);
	playermsg.add<uint32_t>(std::min<uint32_t>(10, icon));
	playermsg.addByte(notify ? 0x01 : 0x00);
	#endif
	playermsg.addByte(status);
	#if GAME_FEATURE_VIP_GROUPS > 0
	playermsg.addByte(0x00);
	#endif
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendVIPEntries()
{
	std::ostringstream query;
	#if GAME_FEATURE_ADDITIONAL_VIPINFO > 0
	query << "SELECT `player_id`, (SELECT `name` FROM `players` WHERE `id` = `player_id`) AS `name`, `description`, `icon`, `notify` FROM `account_viplist` WHERE `account_id` = " << player->getAccount();
	#else
	query << "SELECT `player_id`, (SELECT `name` FROM `players` WHERE `id` = `player_id`) AS `name` FROM `account_viplist` WHERE `account_id` = " << player->getAccount();
	#endif

	using ProtocolGameWeak_ptr = std::weak_ptr<ProtocolGame>;
	ProtocolGameWeak_ptr protocolGameWeak = std::weak_ptr<ProtocolGame>(getThis());

	std::function<void(DBResult_ptr, bool)> callback = [protocolGameWeak](DBResult_ptr result, bool) {
		if (auto client = protocolGameWeak.lock()) {
			if (result && !client->isConnectionExpired() && client->player) {
				do {
					VipStatus_t vipStatus = VIPSTATUS_ONLINE;
					uint32_t vipGuid = result->getNumber<uint32_t>("player_id");

					Player* vipPlayer = g_game().getPlayerByGUID(vipGuid);
					if (!vipPlayer || vipPlayer->isInGhostMode() || client->player->isAccessPlayer()) {
						vipStatus = VIPSTATUS_OFFLINE;
					}

					#if GAME_FEATURE_ADDITIONAL_VIPINFO > 0
					client->sendVIP(vipGuid, result->getString("name"), result->getString("description"), result->getNumber<uint32_t>("icon"), (result->getNumber<uint16_t>("notify") != 0), vipStatus);
					#else
					client->sendVIP(vipGuid, result->getString("name"), vipStatus);
					#endif
				} while (result->next());
			}
		}
	};
	g_databaseTasks().addTask(query.str(), callback, true);
}

void ProtocolGame::sendSpellCooldown(uint8_t spellId, uint32_t time)
{
	playermsg.reset();
	playermsg.addByte(0xA4);
	playermsg.addByte(spellId);
	playermsg.add<uint32_t>(time);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendSpellGroupCooldown(SpellGroup_t groupId, uint32_t time)
{
	playermsg.reset();
	playermsg.addByte(0xA5);
	playermsg.addByte(groupId);
	playermsg.add<uint32_t>(time);
	writeToOutputBuffer(playermsg);
}

void ProtocolGame::sendModalWindow(const ModalWindow& modalWindow)
{
	playermsg.reset();
	playermsg.addByte(0xFA);

	playermsg.add<uint32_t>(modalWindow.id);
	playermsg.addString(modalWindow.title);
	playermsg.addString(modalWindow.message);

	playermsg.addByte(modalWindow.buttons.size());
	for (const auto& it : modalWindow.buttons) {
		playermsg.addString(it.first);
		playermsg.addByte(it.second);
	}

	playermsg.addByte(modalWindow.choices.size());
	for (const auto& it : modalWindow.choices) {
		playermsg.addString(it.first);
		playermsg.addByte(it.second);
	}

	playermsg.addByte(modalWindow.defaultEscapeButton);
	playermsg.addByte(modalWindow.defaultEnterButton);
	playermsg.addByte(modalWindow.priority ? 0x01 : 0x00);

	writeToOutputBuffer(playermsg);
}

////////////// Add common messages
void ProtocolGame::AddCreature(const Creature* creature, bool known, uint32_t remove)
{
	CreatureType_t creatureType = creature->getType();
	if (creature->isHealthHidden()) {
		creatureType = CREATURETYPE_HIDDEN;
	}

	const Player* otherPlayer = creature->getPlayer();
	if (known) {
		playermsg.add<uint16_t>(0x62);
		playermsg.add<uint32_t>(creature->getID());
	} else {
		playermsg.add<uint16_t>(0x61);
		playermsg.add<uint32_t>(remove);
		playermsg.add<uint32_t>(creature->getID());
		playermsg.addByte(creatureType);
		if (creatureType == CREATURETYPE_HIDDEN) {
			playermsg.addString(std::string());
		} else {
			playermsg.addString(creature->getName());
		}
	}

	playermsg.addByte(std::ceil((static_cast<double>(creature->getHealth()) / std::max<int32_t>(creature->getMaxHealth(), 1)) * 100));

	playermsg.addByte(creature->getDirection());
	if (!creature->isInGhostMode() && !creature->isInvisible()) {
		const Outfit_t& outfit = creature->getCurrentOutfit();
		AddOutfit(outfit);
	} else {
		static Outfit_t outfit;
		AddOutfit(outfit);
	}

	LightInfo lightInfo = creature->getCreatureLight();
	playermsg.addByte(player->isAccessPlayer() ? 0xFF : lightInfo.level);
	playermsg.addByte(lightInfo.color);

	#if GAME_FEATURE_NEWSPEED_LAW > 0
	playermsg.add<uint16_t>(creature->getStepSpeed() / 2);
	#else
	playermsg.add<uint16_t>(creature->getStepSpeed());
	#endif

	// TODO: sync with 1240 protocol
	// #if CLIENT_VERSION >= 1240
	// playermsg.addByte(0);//icons
	// #endif

	playermsg.addByte(player->getSkullClient(creature));
	playermsg.addByte(player->getPartyShield(otherPlayer));
	#if GAME_FEATURE_GUILD_EMBLEM > 0
	if (!known) {
		playermsg.addByte(player->getGuildEmblem(otherPlayer));
	}
	#endif

	#if GAME_FEATURE_CREATURE_TYPE > 0
	if (creatureType == CREATURETYPE_MONSTER) {
		const Creature* master = creature->getMaster();
		if (master) {
			const Player* masterPlayer = master->getPlayer();
			if (masterPlayer) {
				if (masterPlayer == player) {
					creatureType = CREATURETYPE_SUMMON_OWN;
				} else {
					creatureType = CREATURETYPE_SUMMON_OTHERS;
				}
			}
		}
	}

	if (creatureType == CREATURETYPE_SUMMON_OTHERS) {
		creatureType = CREATURETYPE_SUMMON_OWN;
	}
	playermsg.addByte(creatureType);
	if (creatureType == CREATURETYPE_SUMMON_OWN) {
		const Creature* master = creature->getMaster();
		if (master) {
			playermsg.add<uint32_t>(master->getID());
		} else {
			playermsg.add<uint32_t>(0);
		}
	}
	else if (creatureType == CREATURETYPE_PLAYER) {
		playermsg.addByte(creature->getPlayer()->getVocation()->getClientId());
	}
	#endif

	#if GAME_FEATURE_CREATURE_ICONS > 0
	playermsg.addByte(creature->getSpeechBubble());
	#endif
	#if GAME_FEATURE_CREATURE_MARK > 0
	playermsg.addByte(0xFF); // MARK_UNMARKED
	#endif
	#if GAME_FEATURE_INSPECTION > 0
	playermsg.addByte(0); // inspection type
	#endif

	playermsg.addByte(player->canWalkthroughEx(creature) ? 0x00 : 0x01);
}

void ProtocolGame::AddPlayerStats()
{
	playermsg.addByte(0xA0);

	#if GAME_FEATURE_DOUBLE_HEALTH > 0
	playermsg.add<uint32_t>(player->getHealth());
	playermsg.add<uint32_t>(player->getMaxHealth());
	#else
	playermsg.add<uint16_t>(std::min<int32_t>(player->getHealth(), std::numeric_limits<uint16_t>::max()));
	playermsg.add<uint16_t>(std::min<int32_t>(player->getMaxHealth(), std::numeric_limits<uint16_t>::max()));
	#endif

	#if GAME_FEATURE_DOUBLE_CAPACITY > 0
	playermsg.add<uint32_t>(player->getFreeCapacity());
	#else
	playermsg.add<uint16_t>(player->getFreeCapacity());
	#endif

	#if GAME_FEATURE_DOUBLE_EXPERIENCE > 0
	playermsg.add<uint64_t>(player->getExperience());
	#else
	playermsg.add<uint32_t>(std::min<uint64_t>(player->getExperience(), std::numeric_limits<uint32_t>::max()));
	#endif

	playermsg.add<uint16_t>(player->getLevel());
	playermsg.addByte(player->getLevelPercent());

	#if GAME_FEATURE_EXPERIENCE_BONUS > 0
	#if GAME_FEATURE_DETAILED_EXPERIENCE_BONUS > 0
	playermsg.add<uint16_t>(100); // base xp gain rate
	playermsg.add<uint16_t>(0); // low level bonus
	playermsg.add<uint16_t>(0); // xp boost
	playermsg.add<uint16_t>(100); // stamina multiplier (100 = x1.0)
	#else
	playermsg.addDouble(0.0);
	#endif
	#endif

	#if GAME_FEATURE_DOUBLE_HEALTH > 0
	playermsg.add<uint32_t>(player->getMana());
	playermsg.add<uint32_t>(player->getMaxMana());
	#else
	playermsg.add<uint16_t>(std::min<int32_t>(player->getMana(), std::numeric_limits<uint16_t>::max()));
	playermsg.add<uint16_t>(std::min<int32_t>(player->getMaxMana(), std::numeric_limits<uint16_t>::max()));
	#endif

	playermsg.addByte(player->getSoul());

	#if GAME_FEATURE_STAMINA > 0
	playermsg.add<uint16_t>(player->getStaminaMinutes());
	#endif

	#if GAME_FEATURE_BASE_SKILLS > 0
	playermsg.add<uint16_t>(player->getBaseSpeed() / 2);
	#endif

	#if GAME_FEATURE_REGENERATION_TIME > 0
	Condition* condition = player->getCondition(CONDITION_REGENERATION);
	playermsg.add<uint16_t>(condition ? condition->getTicks() / 1000 : 0x00);
	#endif

	#if GAME_FEATURE_OFFLINE_TRAINING > 0
	playermsg.add<uint16_t>(player->getOfflineTrainingTime() / 60 / 1000);
	#endif

	#if GAME_FEATURE_DETAILED_EXPERIENCE_BONUS > 0
	playermsg.add<uint16_t>(0); // xp boost time (seconds)
	playermsg.addByte(0); // enables exp boost in the store
	#endif
}

void ProtocolGame::AddPlayerSkills()
{
	// TODO: improve OTC to accept loyalty bonus - needs to change global otbr too
	bool loyaltyBonus = player->getOperatingSystem() < CLIENTOS_OTCLIENT_LINUX;

	playermsg.addByte(0xA1);
	playermsg.add<uint16_t>(player->getMagicLevel());
	playermsg.add<uint16_t>(player->getBaseMagicLevel());
	if (loyaltyBonus) {
		playermsg.add<uint16_t>(player->getBaseMagicLevel());//loyalty bonus
	}
	playermsg.add<uint16_t>(player->getMagicLevelPercent() * 100);

	for (uint8_t i = SKILL_FIRST; i <= SKILL_LAST; ++i) {
		#if GAME_FEATURE_DOUBLE_SKILLS > 0
		playermsg.add<uint16_t>(player->getSkillLevel(i));
		#else
		playermsg.add<uint8_t>(std::min<uint16_t>(player->getSkillLevel(i), std::numeric_limits<uint8_t>::max()));
		#endif

		#if GAME_FEATURE_BASE_SKILLS > 0
		#if GAME_FEATURE_DOUBLE_SKILLS > 0
		playermsg.add<uint16_t>(player->getBaseSkill(i));
		#else
		playermsg.add<uint8_t>(std::min<uint16_t>(player->getBaseSkill(i), std::numeric_limits<uint8_t>::max()));
		#endif
		#endif

		#if GAME_FEATURE_DOUBLE_PERCENT_SKILLS > 0
		if (loyaltyBonus) {
			playermsg.add<uint16_t>(player->getBaseSkill(i));//loyalty bonus
		}
		playermsg.add<uint16_t>(static_cast<uint16_t>(player->getSkillPercent(i)) * 100);
		#else
		playermsg.addByte(player->getSkillPercent(i));
		#endif
	}

	#if GAME_FEATURE_ADDITIONAL_SKILLS > 0
	for (uint8_t i = SPECIALSKILL_FIRST; i <= SPECIALSKILL_LAST; ++i) {
		#if GAME_FEATURE_DOUBLE_SKILLS > 0
		playermsg.add<uint16_t>(std::min<int32_t>(100, player->varSpecialSkills[i]));
		#else
		playermsg.add<uint8_t>(std::min<int32_t>(100, player->varSpecialSkills[i]));
		#endif
		
		#if GAME_FEATURE_BASE_SKILLS > 0
		#if GAME_FEATURE_DOUBLE_SKILLS > 0
		playermsg.add<uint16_t>(0);
		#else
		playermsg.add<uint8_t>(0);
		#endif
		#endif
	}
	#endif

	playermsg.add<uint32_t>(player->getCapacity());
	playermsg.add<uint32_t>(player->getCapacity());
}

void ProtocolGame::AddOutfit(const Outfit_t& outfit)
{
	#if GAME_FEATURE_LOOKTYPE_U16 > 0
	playermsg.add<uint16_t>(outfit.lookType);
	#else
	playermsg.addByte(outfit.lookType);
	#endif
	if (outfit.lookType != 0) {
		playermsg.addByte(outfit.lookHead);
		playermsg.addByte(outfit.lookBody);
		playermsg.addByte(outfit.lookLegs);
		playermsg.addByte(outfit.lookFeet);
		#if GAME_FEATURE_ADDONS > 0
		playermsg.addByte(outfit.lookAddons);
		#endif
	} else {
		playermsg.addItemId(outfit.lookTypeEx);
	}
	
	#if GAME_FEATURE_MOUNTS > 0
	playermsg.add<uint16_t>(outfit.lookMount);
	#endif
}

void ProtocolGame::AddWorldLight(LightInfo lightInfo)
{
	playermsg.addByte(0x82);
	playermsg.addByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	playermsg.addByte(lightInfo.color);
}

void ProtocolGame::AddCreatureLight(const Creature* creature)
{
	LightInfo lightInfo = creature->getCreatureLight();

	playermsg.addByte(0x8D);
	playermsg.add<uint32_t>(creature->getID());
	playermsg.addByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	playermsg.addByte(lightInfo.color);
}

//tile
void ProtocolGame::RemoveTileThing(const Position& pos, uint32_t stackpos)
{
	if (stackpos >= 10) {
		return;
	}

	playermsg.addByte(0x6C);
	playermsg.addPosition(pos);
	playermsg.addByte(stackpos);
}

void ProtocolGame::MoveUpCreature(const Creature* creature, const Position& newPos, const Position& oldPos)
{
	if (creature != player) {
		return;
	}

	//floor change up
	playermsg.addByte(0xBE);

	//going to surface
	if (newPos.z == 7) {
		int32_t skip = -1;
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), 5, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, 3, skip); //(floor 7 and 6 already set)
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), 4, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, 4, skip);
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), 3, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, 5, skip);
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), 2, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, 6, skip);
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), 1, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, 7, skip);
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), 0, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, 8, skip);
		if (skip >= 0) {
			playermsg.addByte(skip);
			playermsg.addByte(0xFF);
		}
	}
	//underground, going one floor up (still underground)
	else if (newPos.z > 7) {
		int32_t skip = -1;
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), oldPos.getZ() - 3, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, 3, skip);
		if (skip >= 0) {
			playermsg.addByte(skip);
			playermsg.addByte(0xFF);
		}
	}

	//moving up a floor up makes us out of sync
	//west
	playermsg.addByte(0x68);
	GetMapDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 2), newPos.z, 1, CLIENT_MAP_HEIGHT);

	//north
	playermsg.addByte(0x65);
	GetMapDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), newPos.z, CLIENT_MAP_WIDTH, 1);
}

void ProtocolGame::MoveDownCreature(const Creature* creature, const Position& newPos, const Position& oldPos)
{
	if (creature != player) {
		return;
	}

	//floor change down
	playermsg.addByte(0xBF);

	//going from surface to underground
	if (newPos.z == 8) {
		int32_t skip = -1;

		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), newPos.z, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, -1, skip);
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), newPos.z + 1, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, -2, skip);
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), newPos.z + 2, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, -3, skip);
		if (skip >= 0) {
			playermsg.addByte(skip);
			playermsg.addByte(0xFF);
		}
	}
	//going further down
	else if (newPos.z > oldPos.z && newPos.z > 8 && newPos.z < 14) {
		int32_t skip = -1;
		GetFloorDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y - (CLIENT_MAP_HEIGHT_OFFFSET - 1), newPos.z + 2, CLIENT_MAP_WIDTH, CLIENT_MAP_HEIGHT, -3, skip);
		if (skip >= 0) {
			playermsg.addByte(skip);
			playermsg.addByte(0xFF);
		}
	}

	//moving down a floor makes us out of sync
	//east
	playermsg.addByte(0x66);
	GetMapDescription(oldPos.x + CLIENT_MAP_WIDTH_OFFSET, oldPos.y - CLIENT_MAP_HEIGHT_OFFFSET, newPos.z, 1, CLIENT_MAP_HEIGHT);

	//south
	playermsg.addByte(0x67);
	GetMapDescription(oldPos.x - (CLIENT_MAP_WIDTH_OFFSET - 1), oldPos.y + CLIENT_MAP_HEIGHT_OFFFSET, newPos.z, CLIENT_MAP_WIDTH, 1);
}

void ProtocolGame::AddShopItem(const ShopInfo& item)
{
	const ItemType& it = Item::items[item.itemId];
	playermsg.add<uint16_t>(it.clientId);
	if (it.isSplash() || it.isFluidContainer()) {
		playermsg.addByte(serverFluidToClient(item.subType));
	} else {
		playermsg.addByte(0x00);
	}

	playermsg.addString(item.realName);
	playermsg.add<uint32_t>(it.weight);
	playermsg.add<uint32_t>(item.buyPrice);
	playermsg.add<uint32_t>(item.sellPrice);
}

void ProtocolGame::AddItem(uint16_t id, uint8_t count)
{
	const ItemType& it = Item::items[id];

	playermsg.add<uint16_t>(it.clientId);

	if (it.stackable) {
		playermsg.addByte(count);
	} else if (it.isSplash() || it.isFluidContainer()) {
		playermsg.addByte(serverFluidToClient(count));
	}

	#if GAME_FEATURE_QUICK_LOOT > 0
	// if (it.isContainer()) {
	// 	playermsg.addByte(0);
	// }
	#endif

	#if GAME_FEATURE_ITEM_ANIMATION_PHASES > 0
	if (it.isAnimation) {
		playermsg.addByte(0xFE); // random phase (0xFF for async)
	}
	#endif
}

void ProtocolGame::AddItem(const Item* item)
{
	const ItemType& it = Item::items[item->getID()];

	playermsg.add<uint16_t>(it.clientId);
	
	if (it.stackable) {
		playermsg.addByte(std::min<uint16_t>(0xFF, item->getItemCount()));
	} else if (it.isSplash() || it.isFluidContainer()) {
		playermsg.addByte(serverFluidToClient(item->getFluidType()));
	}

	#if GAME_FEATURE_QUICK_LOOT > 0
	// if (it.isContainer()) {
	// 	playermsg.addByte(0);
	// }
	#endif

	#if GAME_FEATURE_ITEM_ANIMATION_PHASES > 0
	if (it.isAnimation) {
		playermsg.addByte(0xFE); // random phase (0xFF for async)
	}
	#endif
}

void ProtocolGame::parseExtendedOpcode(NetworkMessage& msg)
{
	uint8_t opcode = msg.getByte();
	const std::string buffer = msg.getString();

	// process additional opcodes via lua script event
	g_game().playerExtendedOpcode(player, opcode, buffer);
}

SpeakClasses ProtocolGame::translateSpeakClassFromClient(uint8_t talkType)
{
	switch (talkType) {
		case 0x01: return TALKTYPE_SAY;
		case 0x02: return TALKTYPE_WHISPER;
		case 0x03: return TALKTYPE_YELL;
		case 0x04: return TALKTYPE_PRIVATE_FROM;
		case 0x05: return TALKTYPE_PRIVATE_TO;
		case 0x06: return TALKTYPE_CHANNEL_M;
		case 0x07: return TALKTYPE_CHANNEL_Y;
		case 0x08: return TALKTYPE_CHANNEL_O;
		case 0x09: return TALKTYPE_SPELL;
		case 0x0A: return TALKTYPE_PRIVATE_NP;
		case 0x0C: return TALKTYPE_PRIVATE_PN;
		case 0x0D: return TALKTYPE_BROADCAST;
		case 0x0E: return TALKTYPE_CHANNEL_R1;
		case 0x0F: return TALKTYPE_PRIVATE_RED_FROM;
		case 0x10: return TALKTYPE_PRIVATE_RED_TO;
		case 0x24: return TALKTYPE_MONSTER_SAY;
		case 0x25: return TALKTYPE_MONSTER_YELL;
		default: return TALKTYPE_NONE;
	}
}

uint8_t ProtocolGame::translateSpeakClassToClient(SpeakClasses talkType)
{
	switch (talkType) {
		case TALKTYPE_SAY: return 0x01;
		case TALKTYPE_WHISPER: return 0x02;
		case TALKTYPE_YELL: return 0x03;
		case TALKTYPE_PRIVATE_FROM: return 0x04;
		case TALKTYPE_PRIVATE_TO: return 0x05;
		case TALKTYPE_CHANNEL_M: return 0x06;
		case TALKTYPE_CHANNEL_Y: return 0x07;
		case TALKTYPE_CHANNEL_O: return 0x08;
		case TALKTYPE_SPELL: return 0x09;
		case TALKTYPE_PRIVATE_NP: return 0x0A;
		case TALKTYPE_PRIVATE_PN: return 0x0C;
		case TALKTYPE_BROADCAST: return 0x0D;
		case TALKTYPE_CHANNEL_R1: return 0x0E;
		case TALKTYPE_PRIVATE_RED_FROM: return 0x0F;
		case TALKTYPE_PRIVATE_RED_TO: return 0x10;
		case TALKTYPE_MONSTER_SAY: return 0x24;
		case TALKTYPE_MONSTER_YELL: return 0x25;
		case TALKTYPE_BOOSTED_CREATURE: return 0x31;
		default: return TALKTYPE_NONE;
	}
}

uint8_t ProtocolGame::translateMessageClassToClient(MessageClasses messageType)
{
	switch (messageType) {
		case MESSAGE_STATUS_CONSOLE_BLUE: return 0x04;
		case MESSAGE_STATUS_CONSOLE_RED: return 0x0D;
		case MESSAGE_STATUS_DEFAULT: return 0x11;
		case MESSAGE_STATUS_WARNING: return 0x12;
		case MESSAGE_EVENT_ADVANCE: return 0x13;
		case MESSAGE_STATUS_SMALL: return 0x15;
		case MESSAGE_INFO_DESCR: return 0x16;
		case MESSAGE_EVENT_DEFAULT: return 0x1E;
		case MESSAGE_GUILD: return 0x21;
		case MESSAGE_PARTY_MANAGEMENT: return 0x22;
		case MESSAGE_PARTY: return 0x23;
		case MESSAGE_EVENT_ORANGE: return 0x24;
		case MESSAGE_STATUS_CONSOLE_ORANGE: return 0x25;
		case MESSAGE_DAMAGE_DEALT: return 0x17;
		case MESSAGE_DAMAGE_RECEIVED: return 0x18;
		case MESSAGE_MANA: return 0x2B;
		case MESSAGE_HEALED: return 0x19;
		case MESSAGE_EXPERIENCE: return 0x1A;
		case MESSAGE_DAMAGE_OTHERS: return 0x1B;
		case MESSAGE_HEALED_OTHERS: return 0x1C;
		case MESSAGE_EXPERIENCE_OTHERS: return 0x1D;
		case MESSAGE_LOOT: return 0x1F;
		case MESSAGE_LOGIN: return 0x11;
		case MESSAGE_WARNING: return 0x12;
		case MESSAGE_GAME: return 0x13;
		case MESSAGE_GAME_HIGHLIGHT: return 0x14;
		case MESSAGE_FAILURE: return 0x15;
		case MESSAGE_LOOK: return 0x16;
		case MESSAGE_STATUS: return 0x1E;
		case MESSAGE_TRADENPC: return 0x20;
		case MESSAGE_REPORT: return 0x26;
		case MESSAGE_HOTKEY: return 0x27;
		case MESSAGE_TUTORIAL: return 0x28;
		case MESSAGE_THANKYOU: return 0x29;
		case MESSAGE_MARKET: return 0x2A;
		default: return MESSAGE_NONE;
	}
}
