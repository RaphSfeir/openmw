#include "worldbindings.hpp"

#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadalch.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadclas.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadingr.hpp>
#include <components/esm3/loadlevlist.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadprob.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/lua/luastate.hpp>
#include <components/misc/finitevalues.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/action.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/store.hpp"
#include "../mwworld/worldmodel.hpp"

#include "luamanagerimp.hpp"

#include "animationbindings.hpp"
#include "context.hpp"
#include "corebindings.hpp"
#include "mwscriptbindings.hpp"

namespace MWLua
{
    struct CellsStore
    {
    };
}

namespace sol
{
    template <>
    struct is_automagical<MWLua::CellsStore> : std::false_type
    {
    };
}

namespace MWLua
{

    static void checkGameInitialized(LuaUtil::LuaState* lua)
    {
        if (MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_NoGame)
            throw std::runtime_error(
                "This function cannot be used until the game is fully initialized.\n" + lua->debugTraceback());
    }

    static void addWorldTimeBindings(sol::table& api, const Context& context)
    {
        using Misc::FiniteFloat;

        Misc::NotNullPtr<MWBase::World> world = MWBase::Environment::get().getWorld();
        MWWorld::DateTimeManager* timeManager = world->getTimeManager();

        api["advanceTime"] = [context, world](const FiniteFloat hours) {
            if (hours <= 0.0f)
                throw std::runtime_error("Time may only be advanced forward");

            context.mLuaManager->addAction([world, hours] {
                world->advanceTime(hours);
                MWBase::Environment::get().getMechanicsManager()->fastForwardAi();
            });
        };

        api["setGameTimeScale"] = [timeManager](const FiniteFloat scale) { timeManager->setGameTimeScale(scale); };
        api["setSimulationTimeScale"] = [context, timeManager](const FiniteFloat scale) {
            context.mLuaManager->addAction([scale, timeManager] { timeManager->setSimulationTimeScale(scale); });
        };

        api["pause"]
            = [timeManager](sol::optional<std::string_view> tag) { timeManager->pause(tag.value_or("paused")); };
        api["unpause"]
            = [timeManager](sol::optional<std::string_view> tag) { timeManager->unpause(tag.value_or("paused")); };
        api["getPausedTags"] = [timeManager](sol::this_state lua) {
            sol::table res(lua, sol::create);
            for (const std::string& tag : timeManager->getPausedTags())
                res[tag] = tag;
            return res;
        };
    }

    static void addCellGetters(sol::table& api, const Context& context)
    {
        api["getCellByName"] = [](std::string_view name) {
            return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(name, /*forceLoad=*/false) };
        };
        api["getCellById"] = [](std::string_view stringId) {
            return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(
                ESM::RefId::deserializeText(stringId), /*forceLoad=*/false) };
        };
        api["getExteriorCell"] = [](int x, int y, sol::object cellOrName) {
            ESM::RefId worldspace;
            if (cellOrName.is<GCell>())
                worldspace = cellOrName.as<GCell>().mStore->getCell()->getWorldSpace();
            else if (cellOrName.is<std::string_view>() && !cellOrName.as<std::string_view>().empty())
                worldspace = MWBase::Environment::get()
                                 .getWorldModel()
                                 ->getCell(cellOrName.as<std::string_view>())
                                 .getCell()
                                 ->getWorldSpace();
            else
                worldspace = ESM::Cell::sDefaultWorldspaceId;
            return GCell{ &MWBase::Environment::get().getWorldModel()->getExterior(
                ESM::ExteriorCellLocation(x, y, worldspace), /*forceLoad=*/false) };
        };

        const MWWorld::Store<ESM::Cell>* cells3Store = &MWBase::Environment::get().getESMStore()->get<ESM::Cell>();
        const MWWorld::Store<ESM4::Cell>* cells4Store = &MWBase::Environment::get().getESMStore()->get<ESM4::Cell>();
        auto view = context.sol();
        sol::usertype<CellsStore> cells = view.new_usertype<CellsStore>("Cells");
        cells[sol::meta_function::length]
            = [cells3Store, cells4Store](const CellsStore&) { return cells3Store->getSize() + cells4Store->getSize(); };
        cells[sol::meta_function::index]
            = [cells3Store, cells4Store](const CellsStore&, size_t index) -> sol::optional<GCell> {
            if (index > cells3Store->getSize() + cells4Store->getSize() || index == 0)
                return sol::nullopt;

            index--; // Translate from Lua's 1-based indexing.
            if (index < cells3Store->getSize())
            {
                const ESM::Cell* cellRecord = cells3Store->at(index);
                return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(
                    cellRecord->mId, /*forceLoad=*/false) };
            }
            else
            {
                const ESM4::Cell* cellRecord = cells4Store->at(index - cells3Store->getSize());
                return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(
                    cellRecord->mId, /*forceLoad=*/false) };
            }
        };
        cells[sol::meta_function::pairs] = view["ipairsForArray"].template get<sol::function>();
        cells[sol::meta_function::ipairs] = view["ipairsForArray"].template get<sol::function>();
        api["cells"] = CellsStore{};
    }

    sol::table initWorldPackage(const Context& context)
    {
        sol::table api(context.mLua->unsafeState(), sol::create);

        addCoreTimeBindings(api, context);
        addWorldTimeBindings(api, context);
        addCellGetters(api, context);
        api["mwscript"] = initMWScriptBindings(context);

        ObjectLists* objectLists = context.mObjectLists;
        api["activeActors"] = GObjectList{ objectLists->getActorsInScene() };
        api["players"] = GObjectList{ objectLists->getPlayers() };

        api["getObjectsInRange"] = [](osg::Vec3f position, Misc::FiniteFloat range) -> GObjectList {
            std::vector<MWWorld::Ptr> objects;
            MWBase::Environment::get().getMechanicsManager()->getObjectsInRange(position, range, objects);
            GObjectList objList;
            objList.mIds = std::make_shared<std::vector<ObjectId>>();
            objList.mIds->reserve(objects.size());
            for (const auto& object : objects)
                objList.mIds->push_back(getId(object));
            return objList;
        };
        api["createObject"] = [lua = context.mLua](std::string_view recordId, sol::optional<int> count) -> GObject {
            checkGameInitialized(lua);
            MWWorld::ManualRef mref(*MWBase::Environment::get().getESMStore(), ESM::RefId::deserializeText(recordId));
            const MWWorld::Ptr& ptr = mref.getPtr();
            ptr.getRefData().disable();
            MWWorld::CellStore& cell = MWBase::Environment::get().getWorldModel()->getDraftCell();
            MWWorld::Ptr newPtr = ptr.getClass().copyToCell(ptr, cell, count.value_or(1));
            return GObject(newPtr);
        };
        api["getObjectByFormId"] = [](std::string_view formIdStr) -> GObject {
            ESM::RefId refId = ESM::RefId::deserializeText(formIdStr);
            if (!refId.is<ESM::FormId>())
                throw std::runtime_error("FormId expected, got " + std::string(formIdStr) + "; use core.getFormId");
            return GObject(*refId.getIf<ESM::FormId>());
        };
        api["getObjectsByRecordId"]
            = [](sol::this_state thisState, std::string_view serializedId, std::optional<std::string_view> worldSpace,
                  std::optional<bool> loadedOnly) -> sol::table {
            ESM::RefId id = ESM::RefId::deserializeText(serializedId);
            sol::table objects(thisState, sol::create);
            if (id.empty())
                return objects;
            ESM::RefId worldSpaceId;
            if (worldSpace)
            {
                worldSpaceId = ESM::RefId::deserializeText(*worldSpace);
                if (worldSpaceId.empty())
                    return objects;
            }
            const bool searchUnloaded = !loadedOnly.value_or(false);
            std::vector<MWWorld::Ptr> ptrs
                = MWBase::Environment::get().getWorldModel()->getAll(id, worldSpaceId, searchUnloaded);
            for (const MWWorld::Ptr& ptr : ptrs)
                objects.add(GObject(ptr));
            return objects;
        };

        // Kinematic placement: put an actor exactly where it is told, every frame if
        // needed. `teleport` is the wrong tool for continuous correction (it resets
        // momentum, re-runs cell placement and marks the actor as teleported), and
        // steering an actor there with movement controls has no pathfinding, so a
        // mirrored actor wedges against furniture. Position stays authoritative here
        // while movement controls keep driving the animation.
        api["setActorPosition"]
            = [context](const GObject& object, const osg::Vec3f& pos, sol::optional<float> yaw) {
                  context.mLuaManager->addAction(
                      [object, pos, yaw] {
                          const MWWorld::Ptr ptr = object.ptr();
                          if (!ptr.getClass().isActor())
                              throw std::runtime_error("Actor expected");
                          MWBase::World* world = MWBase::Environment::get().getWorld();
                          world->moveObject(ptr, pos, true, true);
                          if (yaw)
                              world->rotateObject(ptr, osg::Vec3f(0, 0, *yaw), MWBase::RotationFlag_none);
                      },
                      "SetActorPositionAction");
              };

        // mp: with a big modlist, well over a thousand levelled spawn points are
        // rolled per machine off the shared world PRNG, so two players walk into
        // one cave and meet two different bestiaries. Handing the engine the
        // session's campaign seed and PARTY level makes every machine roll the
        // same creature independently: no wire traffic, no spawn-time ownership
        // handshake, and a cell only one player has ever visited still agrees
        // when the other arrives. Called with nil when the session ends, which
        // puts the vanilla dice back.
        api["setLevelledSpawnRule"] = [](sol::optional<sol::table> rule) {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            if (!rule)
            {
                world->setLevelledSpawnRule(std::nullopt);
                return;
            }
            MWBase::World::LevelledSpawnRule r;
            r.mSeed = static_cast<uint32_t>(rule->get_or("seed", 0));
            r.mLevel = rule->get_or("level", 0);
            r.mEpoch = static_cast<uint32_t>(rule->get_or("epoch", 0));
            world->setLevelledSpawnRule(r);
        };

        // mp: THE SPAWN POINTS THE PARTY HAS CLEARED, so a cave they emptied is
        // still empty when they come back.
        //
        // The creature a spawner produces is a runtime object with no portable
        // identity, so its death cannot go in the campaign; the spawner is a
        // content reference and can. The session therefore remembers cleared
        // SPAWNERS and pushes the whole set here, and
        // CreatureLevList::insertObjectRendering consults it before it rolls --
        // the one place a spawn can be prevented rather than undone.
        //
        // Takes a list of { contentFile = <int>, index = <int> } because that is
        // what a RefNum is, and because a Lua number cannot hold the pair: a
        // modlist past 255 plugins overflows the packed form the engine itself
        // refuses to compute for the same reason.
        //
        // WHOLE SET, REPLACING: idempotent, so re-sending is free and a refilled
        // cave needs no separate retraction. An empty list is the vanilla state.
        api["setClearedLevelledSpawns"] = [](sol::optional<sol::table> list) {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            std::vector<ESM::RefNum> cleared;
            if (list)
            {
                cleared.reserve(list->size());
                for (const auto& [_, v] : *list)
                {
                    sol::table e = v.as<sol::table>();
                    ESM::RefNum n;
                    n.mContentFile = e.get_or("contentFile", -1);
                    n.mIndex = static_cast<uint32_t>(e.get_or("index", 0));
                    if (n.hasContentFile())
                        cleared.push_back(n);
                }
            }
            world->setClearedLevelledSpawns(std::move(cleared));
        };

        // Creates a new record in the world database.
        api["createRecord"] = sol::overload(
            [lua = context.mLua](const ESM::Activator& activator) -> const ESM::Activator* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(activator);
            },
            [lua = context.mLua](const ESM::Armor& armor) -> const ESM::Armor* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(armor);
            },
            [lua = context.mLua](const ESM::Clothing& clothing) -> const ESM::Clothing* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(clothing);
            },
            [lua = context.mLua](const ESM::CreatureLevList& list) -> const ESM::CreatureLevList* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(list);
            },
            [lua = context.mLua](const ESM::Book& book) -> const ESM::Book* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(book);
            },
            [lua = context.mLua](const ESM::Enchantment& enchantment) -> const ESM::Enchantment* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(enchantment);
            },
            [lua = context.mLua](const ESM::Ingredient& ingred) -> const ESM::Ingredient* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(ingred);
            },
            [lua = context.mLua](const ESM::ItemLevList& list) -> const ESM::ItemLevList* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(list);
            },
            [lua = context.mLua](const ESM::Miscellaneous& misc) -> const ESM::Miscellaneous* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(misc);
            },
            [lua = context.mLua](const ESM::Potion& potion) -> const ESM::Potion* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(potion);
            },
            [lua = context.mLua](const ESM::Probe& probe) -> const ESM::Probe* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(probe);
            },
            [lua = context.mLua](const ESM::Spell& spell) -> const ESM::Spell* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(spell);
            },
            [lua = context.mLua](const ESM::Static& stat) -> const ESM::Static* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(stat);
            },
            [lua = context.mLua](const ESM::NPC& npc) -> const ESM::NPC* {
                checkGameInitialized(lua);
                if (npc.mId.empty())
                    return MWBase::Environment::get().getESMStore()->insert(npc);
                ESM::NPC copy = npc;
                copy.mId = {};
                return MWBase::Environment::get().getESMStore()->insert(copy);
            },
            // A CLASS, which the player's own sheet needs and nothing else could
            // make. Every other creatable record here describes a thing in the
            // world; a class describes a character, and without it a character
            // imported from another server can only ever be forced into one of
            // the twenty-one presets -- losing the major/minor skills that
            // decide how they level. tableToClass already existed for the
            // content store; this is the same record, insertable at runtime.
            [lua = context.mLua](const ESM::Class& cls) -> const ESM::Class* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(cls);
            },
            [lua = context.mLua](const ESM::Creature& crea) -> const ESM::Creature* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(crea);
            },
            [lua = context.mLua](const ESM::Container& cont) -> const ESM::Container* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(cont);
            },
            [lua = context.mLua](const ESM::Door& cont) -> const ESM::Door* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(cont);
            },
            [lua = context.mLua](const ESM::Weapon& weapon) -> const ESM::Weapon* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(weapon);
            },
            [lua = context.mLua](const ESM::Light& light) -> const ESM::Light* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(light);
            });

        api["_runStandardActivationAction"] = [context](const GObject& object, const GObject& actor) {
            if (!object.ptr().getRefData().activate())
                return;
            context.mLuaManager->addAction(
                [object, actor] {
                    const MWWorld::Ptr& objPtr = object.ptr();
                    const MWWorld::Ptr& actorPtr = actor.ptr();
                    objPtr.getClass().activate(objPtr, actorPtr)->execute(actorPtr);
                },
                "_runStandardActivationAction");
        };
        api["_runStandardUseAction"] = [context](const GObject& object, const GObject& actor, bool force) {
            context.mLuaManager->addAction(
                [object, actor, force] {
                    const MWWorld::Ptr& actorPtr = actor.ptr();
                    const MWWorld::Ptr& objectPtr = object.ptr();
                    if (actorPtr == MWBase::Environment::get().getWorld()->getPlayerPtr())
                        MWBase::Environment::get().getWindowManager()->useItem(objectPtr, force);
                    else
                    {
                        std::unique_ptr<MWWorld::Action> action = objectPtr.getClass().use(objectPtr, force);
                        action->execute(actorPtr, true);
                    }
                },
                "_runStandardUseAction");
        };

        api["vfx"] = initWorldVfxBindings(context);

        return LuaUtil::makeReadOnly(api);
    }
}
