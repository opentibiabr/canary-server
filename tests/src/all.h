#ifndef CANARY_TEST_H
#define CANARY_TEST_H

#include <boost/filesystem.hpp>

// Include main Doctest framework library
#include "doctest/doctest/doctest.h"

// Include main SRC library
#include "../../src/otpch.h"
#include "../../src/actions.h"
#include "../../src/ban.h"
#include "../../src/baseevents.h"
#include "../../src/bed.h"
#include "../../src/chat.h"
#include "../../src/combat.h"
#include "../../src/condition.h"
#include "../../src/configmanager.h"
#include "../../src/connection.h"
#include "../../src/container.h"
#include "../../src/creature.h"
#include "../../src/creatureevent.h"
#include "../../src/cylinder.h"
#include "../../src/database.h"
#include "../../src/databasemanager.h"
#include "../../src/databasetasks.h"
#include "../../src/decay.h"
#include "../../src/depotchest.h"
#include "../../src/depotlocker.h"
#include "../../src/events.h"
#include "../../src/fileloader.h"
#include "../../src/flatbuffers_wrapper_pool.h"
#include "../../src/game.h"
#include "../../src/globalevent.h"
#include "../../src/guild.h"
#include "../../src/groups.h"
#include "../../src/house.h"
#include "../../src/housetile.h"
#include "../../src/inbox.h"
#include "../../src/ioguild.h"
#include "../../src/iologindata.h"
#include "../../src/iomap.h"
#include "../../src/iomapserialize.h"
#include "../../src/iomarket.h"
#include "../../src/item.h"
#include "../../src/items.h"
#include "../../src/luascript.h"
#include "../../src/mailbox.h"
#include "../../src/map.h"
#include "../../src/modules.h"
#include "../../src/monster.h"
#include "../../src/monsters.h"
#include "../../src/mounts.h"
#include "../../src/movement.h"
#include "../../src/networkmessage.h"
#include "../../src/npc.h"
#include "../../src/outfit.h"
#include "../../src/party.h"
#include "../../src/player.h"
#include "../../src/position.h"
#include "../../src/protocol.h"
#include "../../src/protocolgame.h"
#include "../../src/protocollogin.h"
#include "../../src/protocolstatus.h"
#include "../../src/quests.h"
#include "../../src/raids.h"
#include "../../src/rsa.h"
#include "../../src/scripts.h"
#include "../../src/server.h"
#include "../../src/signals.h"
#include "../../src/spawn.h"
#include "../../src/spells.h"
#include "../../src/talkaction.h"
#include "../../src/tasks.h"
#include "../../src/teleport.h"
#include "../../src/thing.h"
#include "../../src/tile.h"
#include "../../src/tools.h"
#include "../../src/trashholder.h"
#include "../../src/vocation.h"
#include "../../src/waitlist.h"
#include "../../src/weapons.h"
#include "../../src/wildcardtree.h"

namespace fs = boost::filesystem;

extern LuaEnvironment g_luaEnvironment;

extern MonsterType type;
extern Player player;
extern Monster monsterA;
extern Monster monsterB;
extern Npc npc;
extern Vocation voc;

#endif