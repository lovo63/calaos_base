/******************************************************************************
 **  Copyright (c) 2006-2014, Calaos. All Rights Reserved.
 **
 **  This file is part of Calaos.
 **
 **  Calaos is free software; you can redistribute it and/or modify
 **  it under the terms of the GNU General Public License as published by
 **  the Free Software Foundation; either version 3 of the License, or
 **  (at your option) any later version.
 **
 **  Calaos is distributed in the hope that it will be useful,
 **  but WITHOUT ANY WARRANTY; without even the implied warranty of
 **  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 **  GNU General Public License for more details.
 **
 **  You should have received a copy of the GNU General Public License
 **  along with Foobar; if not, write to the Free Software
 **  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 **
 ******************************************************************************/
#include "RoomModel.h"

bool IOHitsCompare(const IOBase *lhs, const IOBase *rhs)
{
    return ((IOBase *)lhs)->params["hits"] > ((IOBase *)rhs)->params["hits"];
}

bool IOScenarioCompare(const IOBase *lhs, const IOBase *rhs)
{
    return ((IOBase *)lhs)->params["scenarioPref"] > ((IOBase *)rhs)->params["scenarioPref"];
}

bool RoomHitsCompare(const Room *lhs, const Room *rhs)
{
    return lhs->hits > rhs->hits;
}

RoomModel::RoomModel(CalaosConnection *con):
    connection(con)
{
    connection->getListener()->notify_room_delete.connect(
                sigc::mem_fun(*this, &RoomModel::notifyRoomDel));
    connection->getListener()->notify_room_new.connect(
                sigc::mem_fun(*this, &RoomModel::notifyRoomAdd));
}

RoomModel::~RoomModel()
{
    for_each(rooms.begin(), rooms.end(), Delete());
}

void RoomModel::load()
{
    room_loaded = 0;
    connection->SendCommand("home ?", sigc::mem_fun(*this, &RoomModel::home_get_cb));
}

void RoomModel::home_get_cb(bool success, vector<string> result, void *data)
{
    if (!success) return;

    for (uint i = 1;i < result.size();i++)
    {
        vector<string> tmp;
        split(result[i], tmp, ":", 2);
        int nb;
        if (tmp.size() < 2) continue;
        if (tmp[1].empty()) continue;
        from_string(tmp[1], nb);

        for(int j = 0;j < nb;j++)
        {
            Room *room = new Room(connection, this);
            room->type = tmp[0];

            rooms.push_back(room);

            room_loaded++;
            room->load_done.connect(sigc::mem_fun(*this, &RoomModel::load_room_done));

            string cmd = "room " + room->type + " get " + Utils::to_string(j);
            connection->SendCommand(cmd, sigc::mem_fun(*room, &Room::new_room_cb));
        }
    }

    if (room_loaded <= 0)
        load_done.emit();
}

void RoomModel::load_room_done(Room *room)
{
    room_loaded--;

    cDebug() << "[ROOM load done]";

    if (room_loaded <= 0)
    {
        rooms.sort(RoomHitsCompare);
        cacheScenarios.sort(IOHitsCompare);
        cacheScenariosPref = cacheScenarios;
        cacheScenariosPref.sort(IOScenarioCompare);

        updateRoomType();

        cDebug() << "[ROOM LOAD DONE sending signal]";

        load_done.emit();
    }
}

void Room::new_room_cb(bool success, vector<string> result, void *data)
{
    if (!success) return;

    io_loaded = 0;
    for (uint b = 2;b < result.size();b++)
    {
        vector<string> tmp;
        Utils::split(result[b], tmp, ":", 2);
        if (tmp[0] == "input")
            loadNewIO(tmp[1], IOBase::IO_INPUT);
        else if (tmp[0] == "output")
            loadNewIO(tmp[1], IOBase::IO_OUTPUT);
        else if(tmp[0] == "name")
            name = tmp[1];
        else if(tmp[0] == "hits")
            Utils::from_string(tmp[1], hits);
    }

    if (io_loaded <= 0)
        load_done.emit(this);
}

void Room::loadNewIO(string id, int io_type)
{
    IOBase *io = new IOBase(connection, this, io_type);
    ios.push_back(io);

    io->params.Add("id", id);

    string cmd;
    if (io_type == IOBase::IO_INPUT)
        cmd = "input ";
    else
        cmd = "output ";
    cmd += id + " get";

    io->load_done.connect(sigc::mem_fun(*this, &Room::load_io_done));
    io_loaded++;

    connection->SendCommand(cmd, sigc::mem_fun(*io, &IOBase::new_io_cb));
}

void Room::load_io_done(IOBase *io)
{
    io_loaded--;

    //Put everything in cache so we can use it easily later
    if (io->io_type == IOBase::IO_INPUT)
        model->cacheInputs[io->params["id"]] = io;
    else
        model->cacheOutputs[io->params["id"]] = io;

    if (io->params["chauffage_id"] != "")
        model->chauffageList.push_back(io);

    string _type = io->params["gui_type"];

    if (_type == "scenario" && io->io_type == IOBase::IO_OUTPUT)
    {
        model->cacheScenarios.push_back(io);
    }

    if (_type == "light" ||
        _type == "light_dimmer" ||
        _type == "light_rgb")
    {
        int value;
        from_string(io->params["state"], value);

        if (io->params["state"] == "true" || value > 0)
        {
            RoomIO roomIO;
            roomIO.io = io;
            roomIO.room = this;

            model->cacheLightsOn[io] = roomIO;
        }
    }

    if (_type == "shutter" ||
        _type == "shutter_smart")
    {
        int value = 100;

        vector<string> tokens;
        split(io->params["state"], tokens);
        if (tokens.size() > 1)
            from_string(tokens[1], value);

        if (io->params["state"] == "false" || value < 100 || io->params["state"] == "down")
        {
            RoomIO roomIO;
            roomIO.io = io;
            roomIO.room = this;

            model->cacheShuttersUp[io] = roomIO;
        }
    }

    if (_type == "time_range")
    {
        io->loadPlage();
    }

    if (io_loaded <= 0)
    {
        ios.sort(IOHitsCompare);

        updateVisibleIO();

        load_done.emit(this);
    }
}

void Room::updateVisibleIO()
{
    visible_ios.clear();
    scenario_ios.clear();

    list<IOBase *>::iterator it = ios.begin();
    for (;it != ios.end();it++)
    {
        IOBase *io = *it;

        if (io->params["gui_type"] != "scenario" &&
            io->params.Exists("auto_scenario"))
            continue;

        if (io->params["gui_type"] == "light" ||
            (io->params["gui_type"] == "var_bool" && io->io_type == IOBase::IO_OUTPUT) ||
            (io->params["gui_type"] == "var_int" && io->io_type == IOBase::IO_OUTPUT) ||
            (io->params["gui_type"] == "var_string" && io->io_type == IOBase::IO_OUTPUT) ||
            io->params["gui_type"] == "temp" ||
            (io->params["gui_type"] == "scenario" && io->io_type == IOBase::IO_OUTPUT) ||
            io->params["gui_type"] == "analog_in" ||
            io->params["gui_type"] == "light_dimmer" ||
            io->params["gui_type"] == "light_rgb" ||
            io->params["gui_type"] == "shutter" ||
            io->params["gui_type"] == "shutter_smart" ||
            io->params["gui_type"] == "analog_out"  ||
            io->params["gui_type"] == "string_out")
        {
            if (io->params["visible"] == "true")
                visible_ios.push_back(io);

            scenario_ios.push_back(io);
        }

        if (io->params["gui_type"] == "camera_output" ||
            io->params["gui_type"] == "audio_output")
        {
            scenario_ios.push_back(io);
        }
    }

    visible_ios.sort(IOHitsCompare);
    scenario_ios.sort(IOHitsCompare);
}

IOBase *RoomModel::getChauffageForType(string type)
{
    list<Room *> l;
    list<Room *>::iterator it = rooms_type.begin();

    for (;it != rooms_type.end();it++)
    {
        if ((*it)->type == type)
            l.push_back((*it));
    }

    it = l.begin();
    l.sort(RoomHitsCompare);

    for (;it != l.end();it++)
    {
        IOBase *io = (*it)->getChauffage();

        if (io)
            return io;
    }

    return NULL;
}

IOBase *Room::getChauffage()
{
    list<IOBase *>::iterator it = visible_ios.begin();
    for (;it != visible_ios.end();it++)
    {
        IOBase *io = (*it);
        if (io->params["gui_type"] == "temp")
            return io;
    }

    return NULL;
}

void IOBase::new_io_cb(bool success, vector<string> result, void *data)
{
    if (!success) return;

    for (uint b = 1;b < result.size();b++)
    {
        vector<string> tmp;
        Utils::split(result[b], tmp, ":", 2);

        if (tmp.size() < 2) continue;

        params.Add(tmp[0], tmp[1]);
    }

    load_done.emit(this);
}

void RoomModel::updateRoomType()
{
    rooms_type.clear();

    list<Room *>::iterator it, itr;
    for (it = rooms.begin(); it != rooms.end();it++)
    {
        Room *room = *it;

        if (room->type == "Internal")
            continue;

        bool found = false;
        for (itr = rooms_type.begin(); itr != rooms_type.end();itr++)
        {
            if ((*itr)->type == room->type)
                found = true;
        }
        if (found)
            continue;

        rooms_type.push_back(room);
    }
}

void IOBase::sendAction(string command)
{
    string cmd;

    if (io_type == IO_INPUT)
        cmd = "input ";
    else
        cmd = "output ";

    cmd += params["id"] + " set " + url_encode(command);

    connection->SendCommand(cmd, sigc::mem_fun(*this, &IOBase::sendAction_cb));
}

void IOBase::sendUserCommand(string command, CommandDone_cb callback, void *data)
{
    string cmd;

    if (io_type == IO_INPUT)
        cmd = "input ";
    else
        cmd = "output ";

    cmd += params["id"] + " " + command;

    connection->SendCommand(cmd, callback, data);
}

void IOBase::sendAction_cb(bool success, vector<string> result, void *data)
{
    //do nothing...
}

void IOBase::notifyChange(string notif)
{
    vector<string> tok;
    split(notif, tok);

    if (io_type == IO_INPUT && params["gui_type"] == "time_range" &&
        tok[0] == "input_range_change")
    {
        //Reload InPlageHoraire
        loadPlage();
        io_changed.emit();

        return;
    }

    if ((io_type == IO_INPUT && tok[0] != "input") ||
        (io_type == IO_OUTPUT && tok[0] != "output"))
        return;

    if (tok.size() < 3) return;

    for_each(tok.begin(), tok.end(), UrlDecode());
    vector<string> p;
    split(tok[2], p, ":", 2);

    if (params["gui_type"] == "scenario")
    {
        if (params["ioBoolState"] == tok[1] && p[0] == "state")
        {
            params.Add("state", p[1]);
            io_changed.emit();

            return;
        }
    }

    if (tok[1] == params["id"])
    {
        params.Add(p[0], p[1]);
        io_changed.emit();

        if (p[0] == "visible")
            room->updateVisibleIO(); //update visibility

        checkCacheChange();

        //Also update WITemp if we change consigne
        if (params["chauffage_id"] != "" && params["gui_type"] == "var_int")
        {
            IOBase *temp = room->model->getTempFromConsigne(this);
            if (temp)
                temp->io_changed.emit();
        }
    }
}

void IOBase::checkCacheChange()
{
    if (params["gui_type"] == "light" ||
        params["gui_type"] == "light_dimmer" ||
        params["gui_type"] == "light_rgb")
    {
        int value;
        from_string(params["state"], value);

        RoomIOCache::iterator it = room->model->cacheLightsOn.find(this);

        if (params["state"] == "true" || value > 0)
        {
            if (it == room->model->cacheLightsOn.end())
            {
                RoomIO roomIO;
                roomIO.io = this;
                roomIO.room = room;

                room->model->cacheLightsOn[this] = roomIO;
                room->model->lights_on_changed.emit(room->model->cacheLightsOn.size());
            }
        }
        else
        {
            if (it != room->model->cacheLightsOn.end())
            {
                room->model->cacheLightsOn.erase(it);
                room->model->lights_on_changed.emit(room->model->cacheLightsOn.size());
            }
        }
    }

    if (params["gui_type"] == "shutter" ||
        params["gui_type"] == "shutter_smart")
    {
        int value = 100;

        vector<string> tokens;
        split(params["state"], tokens);
        if (tokens.size() > 1)
            from_string(tokens[1], value);

        RoomIOCache::iterator it = room->model->cacheShuttersUp.find(this);

        if (params["state"] == "false" || value < 100 || params["state"] == "down")
        {
            if (it == room->model->cacheShuttersUp.end())
            {
                RoomIO roomIO;
                roomIO.io = this;
                roomIO.room = room;

                room->model->cacheShuttersUp[this] = roomIO;
                room->model->shutters_up_changed.emit(room->model->cacheShuttersUp.size());
            }
        }
        else
        {
            if (it != room->model->cacheShuttersUp.end())
            {
                room->model->cacheShuttersUp.erase(it);
                room->model->shutters_up_changed.emit(room->model->cacheShuttersUp.size());
            }
        }
    }
}

void Room::notifyChange(string notif)
{
    vector<string> tok;
    split(notif, tok);
    Params p;

    for (unsigned int i = 0;i < tok.size();i++)
    {
        vector<string> t;
        split(url_decode(tok[i]), t, ":", 2);

        for_each(t.begin(), t.end(), UrlDecode());

        p.Add(t[0], t[1]);
    }

    if (p.Exists("input_del") &&
        p["room_name"] == name &&
        p["room_type"] == type)
    {
        //an input is deleted from this room
        map<string, IOBase *>::const_iterator it = model->getCacheInputs().find(p["input_del"]);
        if (it != model->getCacheInputs().end())
        {
            IOBase *io = it->second;
            io->room = NULL;
            ios.erase(find(ios.begin(), ios.end(), io));
            ios.sort(IOHitsCompare);
            updateVisibleIO();
        }
    }

    if (p.Exists("output_del") &&
        p["room_name"] == name &&
        p["room_type"] == type)
    {
        //an output is deleted from this room
        map<string, IOBase *>::const_iterator it = model->getCacheOutputs().find(p["output_del"]);
        if (it != model->getCacheOutputs().end())
        {
            IOBase *io = it->second;
            io->room = NULL;
            ios.erase(find(ios.begin(), ios.end(), io));
            ios.sort(IOHitsCompare);
            updateVisibleIO();
        }
    }

    if (p.Exists("input_add") &&
        p["room_name"] == name &&
        p["room_type"] == type)
    {
        //an input is deleted from this room
        map<string, IOBase *>::const_iterator it = model->getCacheInputs().find(p["input_add"]);
        if (it != model->getCacheInputs().end())
        {
            IOBase *io = it->second;
            io->room = this;
            ios.push_back(io);
            ios.sort(IOHitsCompare);
            updateVisibleIO();
        }
    }

    if (p.Exists("output_add") &&
        p["room_name"] == name &&
        p["room_type"] == type)
    {
        //an output is deleted from this room
        map<string, IOBase *>::const_iterator it = model->getCacheOutputs().find(p["output_add"]);
        if (it != model->getCacheOutputs().end())
        {
            IOBase *io = it->second;
            io->room = this;
            ios.push_back(it->second);
            ios.sort(IOHitsCompare);
            updateVisibleIO();
        }
    }

    if (p["old_room_name"] == name && p["room_type"] == type)
    {
        name = p["new_room_name"];
        room_changed.emit();
    }
    else if (p["old_room_type"] == type && p["room_name"] == name)
    {
        type = p["new_room_type"];
        room_changed.emit();
    }
    else if (p["room_type"] == type && p["room_name"] == name)
    {
        from_string(p["new_room_hits"], hits);

        room_changed.emit();
    }
}

void RoomModel::notifyRoomChange(string notif)
{
    vector<string> tok;
    split(notif, tok);
    Params p;

    for (unsigned int i = 0;i < tok.size();i++)
    {
        vector<string> t;
        split(tok[i], t, ":", 2);

        for_each(t.begin(), t.end(), UrlDecode());

        p.Add(t[0], t[1]);
    }

    list<Room *>::iterator it, itr;
    for (it = rooms.begin(); it != rooms.end();it++)
    {
        Room *room = *it;

        if (p["room_type"] == room->type && p["room_name"] == room->name)
        {
            from_string(p["new_room_hits"], room->hits);

            rooms.sort(RoomHitsCompare);
            updateRoomType();
        }
    }
}

void Room::notifyIOAdd(string notif)
{
    vector<string> tok, split_id, split_room_name, split_room_type;
    split(notif, tok);

    if (tok.size() < 4) return;

    split(url_decode(tok[1]), split_id, ":", 2);
    split(url_decode(tok[2]), split_room_name, ":", 2);
    split(url_decode(tok[3]), split_room_type, ":", 2);

    if (split_id.size() < 2 ||
        split_room_name.size() < 2 ||
        split_room_type.size() < 2)
        return;

    if (name != split_room_name[1] || type != split_room_type[1])
        return;

    if (tok[0] == "new_input")
        loadNewIOFromNotif(split_id[1], IOBase::IO_INPUT);
    else if (tok[0] == "new_output")
        loadNewIOFromNotif(split_id[1], IOBase::IO_OUTPUT);
}

void Room::loadNewIOFromNotif(string id, int io_type)
{
    IOBase *io = new IOBase(connection, this, io_type);
    ios.push_back(io);

    io->params.Add("id", id);

    string cmd;
    if (io_type == IOBase::IO_INPUT)
        cmd = "input ";
    else
        cmd = "output ";
    cmd += id + " get";

    io->load_done.connect(sigc::mem_fun(*this, &Room::load_io_notif_done));

    connection->SendCommand(cmd, sigc::mem_fun(*io, &IOBase::new_io_cb));
}

void Room::load_io_notif_done(IOBase *io)
{
    //Put everything in cache so we can use it easily later
    if (io->io_type == IOBase::IO_INPUT)
        model->cacheInputs[io->params["id"]] = io;
    else
        model->cacheOutputs[io->params["id"]] = io;

    if (io->params["chauffage_id"] != "")
        model->chauffageList.push_back(io);

    string _type = io->params["gui_type"];
    if (_type == "scenario" && io->io_type == IOBase::IO_OUTPUT)
    {
        model->cacheScenarios.push_back(io);
        //Sort cached scenarios again
        model->cacheScenariosPref = model->cacheScenarios;
        model->cacheScenariosPref.sort(IOScenarioCompare);
    }

    if (_type == "light" ||
        _type == "light_dimmer" ||
        _type == "light_rgb")
    {
        int value;
        from_string(io->params["state"], value);

        if (io->params["state"] == "true" || value > 0)
        {
            RoomIO roomIO;
            roomIO.io = io;
            roomIO.room = this;

            model->cacheLightsOn[io] = roomIO;
        }
    }

    if (_type == "shutter" ||
        _type == "shutter_smart")
    {
        int value = 100;

        vector<string> tokens;
        split(io->params["state"], tokens);
        if (tokens.size() > 1)
            from_string(tokens[1], value);

        if (io->params["state"] == "false" || value < 100 || io->params["state"] == "down")
        {
            RoomIO roomIO;
            roomIO.io = io;
            roomIO.room = this;

            model->cacheShuttersUp[io] = roomIO;
        }
    }

    ios.sort(IOHitsCompare);

    updateVisibleIO();

    io_added.emit(io);
}

void Room::notifyIODel(string notif)
{
    vector<string> tok, split_room_name, split_room_type;
    split(notif, tok);

    if (tok.size() < 4) return;

    split(url_decode(tok[2]), split_room_name, ":", 2);
    split(url_decode(tok[3]), split_room_type, ":", 2);

    if (split_room_name.size() < 2 ||
        split_room_type.size() < 2)
        return;

    if (name != split_room_name[1] || type != split_room_type[1])
        return;

    list<IOBase *>::iterator it;
    for (it = ios.begin(); it != ios.end();it++)
    {
        IOBase *io = (*it);

        if (io->params["id"] == tok[1])
        {
            //Remove from cache
            if (tok[0] == "delete_input" && io->io_type == IOBase::IO_INPUT)
            {
                model->cacheInputs.erase(model->cacheInputs.find(io->params["id"]));
            }
            else if (tok[0] == "delete_output" && io->io_type == IOBase::IO_OUTPUT)
            {
                model->cacheOutputs.erase(model->cacheOutputs.find(io->params["id"]));
                if (io->params["gui_type"] == "scenario")
                {
                    model->cacheScenarios.erase(find(model->cacheScenarios.begin(), model->cacheScenarios.end(), io));
                    //Sort cached scenarios again
                    model->cacheScenariosPref = model->cacheScenarios;
                    model->cacheScenariosPref.sort(IOScenarioCompare);
                }
            }
            else
            {
                continue;
            }

            list<IOBase *>::iterator cit;
            for (cit = model->chauffageList.begin();cit != model->chauffageList.end();cit++)
            {
                if (*cit == io)
                {
                    model->chauffageList.erase(cit);
                    break;
                }
            }

            if (io->params["gui_type"] == "shutter" ||
                io->params["gui_type"] == "shutter_smart")
            {
                RoomIOCache::iterator sit = model->cacheShuttersUp.find(io);
                if (sit != model->cacheShuttersUp.end())
                    model->cacheShuttersUp.erase(sit);
            }

            if (io->params["gui_type"] == "light" ||
                io->params["gui_type"] == "light_dimmer" ||
                io->params["gui_type"] == "light_rgb")
            {
                RoomIOCache::iterator sit = model->cacheShuttersUp.find(io);
                if (sit != model->cacheShuttersUp.end())
                    model->cacheShuttersUp.erase(sit);
            }

            io_deleted.emit(io);

            delete io;
            ios.erase(it);

            updateVisibleIO();

            break;
        }
    }
}

void RoomModel::notifyRoomAdd(string notif)
{
    vector<string> tok;
    split(notif, tok);
    Params p;

    for (unsigned int i = 0;i < tok.size();i++)
    {
        vector<string> t;
        split(tok[i], t, ":", 2);

        for_each(t.begin(), t.end(), UrlDecode());

        p.Add(t[0], t[1]);
    }

    Room *room = new Room(connection, this);
    room->type = p["type"];
    room->name = p["name"];
    room->hits = 0;

    rooms.push_back(room);
    rooms.sort(RoomHitsCompare);
    updateRoomType();

    room_added.emit(room);
}

void RoomModel::notifyRoomDel(string notif)
{
    vector<string> tok;
    split(notif, tok);
    Params p;

    for (unsigned int i = 0;i < tok.size();i++)
    {
        vector<string> t;
        split(tok[i], t, ":", 2);

        for_each(t.begin(), t.end(), UrlDecode());

        p.Add(t[0], t[1]);
    }

    list<Room *>::iterator it, itr;
    for (it = rooms.begin(); it != rooms.end();it++)
    {
        Room *room = *it;

        if (p["type"] == room->type && p["name"] == room->name)
        {
            room_deleted.emit(room);
            rooms.erase(it);

            delete room;

            break;
        }
    }
}

list<Room *> RoomModel::getRoomsForType(string type)
{
    list<Room *> rtypes;

    list<Room *>::iterator it, itr;
    for (it = rooms.begin(); it != rooms.end();it++)
    {
        Room *room = *it;

        if (room->type != type)
            continue;

        rtypes.push_back(room);
    }

    rtypes.sort(RoomHitsCompare);

    return rtypes;
}

IOBase *RoomModel::getConsigneFromTemp(IOBase *temp)
{
    if (!temp || temp->params["gui_type"] != "temp" || temp->params["chauffage_id"] == "")
        return NULL;

    list<IOBase *>::iterator it;
    for (it = chauffageList.begin();it != chauffageList.end();it++)
    {
        IOBase *io = (*it);

        if (io->params["gui_type"] == "var_int" &&
            io->params["chauffage_id"] == temp->params["chauffage_id"])
            return io;
    }

    return NULL;
}

IOBase *RoomModel::getTempFromConsigne(IOBase *consigne)
{
    if (!consigne || consigne->params["gui_type"] != "var_int" || consigne->params["chauffage_id"] == "")
        return NULL;

    list<IOBase *>::iterator it;
    for (it = chauffageList.begin();it != chauffageList.end();it++)
    {
        IOBase *io = (*it);

        if (io->params["gui_type"] == "var_int" &&
            io->params["chauffage_id"] == consigne->params["chauffage_id"])
            return io;
    }

    return NULL;
}

map<Room *, list<IOBase *> > RoomModel::getLightsOnForRooms()
{
    map<Room *, list<IOBase *> > lmap;

    RoomIOCache::iterator it = cacheLightsOn.begin();
    for (;it != cacheLightsOn.end();it++)
    {
        RoomIO &rio = (*it).second;

        if (lmap.find(rio.room) == lmap.end())
        {
            list<IOBase *> l;
            l.push_back(rio.io);
            lmap[rio.room] = l;
        }
        else
        {
            list<IOBase *> &l = lmap[rio.room];
            l.push_back(rio.io);
        }
    }

    return lmap;
}

map<Room *, list<IOBase *> > RoomModel::getShuttersUpForRooms()
{
    map<Room *, list<IOBase *> > lmap;

    RoomIOCache::iterator it = cacheShuttersUp.begin();
    for (;it != cacheShuttersUp.end();it++)
    {
        RoomIO &rio = (*it).second;

        if (lmap.find(rio.room) == lmap.end())
        {
            list<IOBase *> l;
            l.push_back(rio.io);
            lmap[rio.room] = l;
        }
        else
        {
            list<IOBase *> &l = lmap[rio.room];
            l.push_back(rio.io);
        }
    }

    return lmap;
}

string IOBase::getIconForIO()
{
    if (params["gui_type"] == "light" ||
        params["gui_type"] == "light_dimmer" ||
        params["gui_type"] == "light_rgb")
        return "calaos/icons/element/simple/light";
    else if (params["gui_type"] == "scenario")
        return "calaos/icons/element/simple/scenario";
    else if (params["gui_type"] == "var_bool")
        return "calaos/icons/element/simple/internal_bool";
    else if (params["gui_type"] == "var_int")
        return "calaos/icons/element/simple/internal_int";
    else if (params["gui_type"] == "var_string" || params["gui_type"] == "string_out")
        return "calaos/icons/element/simple/internal_string";
    else if (params["gui_type"] == "shutter" ||
             params["gui_type"] == "shutter_smart")
        return "calaos/icons/element/simple/volet";
    else if (params["gui_type"] == "analog_in" ||
             params["gui_type"] == "analog_out")
        return "calaos/icons/element/simple/analog";
    else if (params["gui_type"] == "temp")
        return "calaos/icons/element/simple/temp";
    else if (params["gui_type"] == "camera_output")
        return "calaos/icons/element/simple/camera";
    else if (params["gui_type"] == "audio_output")
        return "calaos/icons/element/simple/music";

    return "";
}

void IOBase::getRGBValueFromState(int &r, int &g, int &b)
{
    int val = 0;
    string state = params["state"];

    if (Utils::is_of_type<int>(state))
    {
        from_string(state, val);
    }

    r = ((val >> 16) * 100) / 255;
    g = (((val >> 8) & 0x0000FF) * 100) / 255;
    b = ((val & 0x0000FF) * 100) / 255;
}

int IOBase::computeStateFromRGBValue(int r, int g, int b)
{
    int val = (((int)(r * 255 / 100)) << 16) +
              (((int)(g * 255 / 100)) << 8) +
              ((int)(b * 255 / 100));

    return val;
}

double IOBase::getDaliValueFromState()
{
    double val = 0.0;
    string state = params["state"];

    if (Utils::is_of_type<double>(state))
    {
        from_string(state, val);
    }
    else if (state.compare(0, 4, "set ") == 0)
    {
        string s = state;
        s.erase(0, 4);
        from_string(s, val);
    }
    else if (state == "true")
    {
        val = 100.0;
    }
    else if (state == "false")
    {
        val = 0.0;
    }

    return val;
}

int IOBase::getPercentVoletSmart()
{
    vector<string> tokens;
    int percent;

    Utils::split(params["state"], tokens);

    if (tokens.size() == 2)
        from_string(tokens[1], percent);
    else
        percent = 0;

    return percent;
}

string IOBase::getStatusVoletSmart()
{
    vector<string> tokens;
    Utils::split(params["state"], tokens);

    if (tokens.size() > 0)
        return tokens[0];

    return "";
}

vector<IOActionList> IOBase::getActionList()
{
    vector<IOActionList> v;

    if (params["gui_type"] == "light")
    {
        v.push_back(IOActionList("true", "Allumer la lumière", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("false", "Eteindre la lumière", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("toggle", "Inverser l'état de la lumière", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("impulse loop %1 %1 %1 %1", "Faire clignoter la lumière", IOActionList::ACTION_TIME_MS));
        v.push_back(IOActionList("impulse %1", "Allumer la lumière pendant X secondes", "Allumer la lumière pendant %1", IOActionList::ACTION_TIME_MS));
    }
    else if (params["gui_type"] == "light_dimmer")
    {
        v.push_back(IOActionList("true", "Allumer la lumière", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("false", "Eteindre la lumière", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("toggle", "Inverser l'état de la lumière", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("set %1", "Mettre la lumière à X pourcent", "Mettre la lumière à %1%", IOActionList::ACTION_SLIDER));
        v.push_back(IOActionList("set off %1", "Mettre la lumière sans allumer à X pourcent", "Mettre la lumière à %1% sans allumer", IOActionList::ACTION_SLIDER));
        v.push_back(IOActionList("up %1", "Augmenter l'intensité de X pourcent", "Augmenter l'intensité de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("down %1", "Baisser l'intensité de X pourcent", "Baisser l'intensité de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("impulse loop %1 %1 %1 %1", "Faire clignoter la lumière", IOActionList::ACTION_TIME_MS));
        v.push_back(IOActionList("impulse %1", "Allumer la lumière pendant X secondes", "Allumer la lumière pendant %1", IOActionList::ACTION_TIME_MS));
    }
    else if (params["gui_type"] == "light_rgb")
    {
        v.push_back(IOActionList("true", "Allumer la lumière", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("false", "Eteindre la lumière", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("toggle", "Inverser l'état de la lumière", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("set %1", "Choisir une couleur", "Mettre la couleur", IOActionList::ACTION_COLOR));
        v.push_back(IOActionList("set_red %1", "Choisir l'intensité du rouge", "Mettre le rouge à %1%", IOActionList::ACTION_SLIDER));
        v.push_back(IOActionList("up_red %1", "Augmenter le rouge de X pourcent", "Augmenter le rouge de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("down_red %1", "Baisser le rouge de X pourcent", "Baisser le rouge de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("set_green %1", "Choisir l'intensité du vert", "Mettre le vert à %1%", IOActionList::ACTION_SLIDER));
        v.push_back(IOActionList("up_green %1", "Augmenter le vert", "Augmenter le vert de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("down_green %1", "Baisser le vert", "Baisser le vert de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("set_blue %1", "Choisir l'intensité du bleu", "Mettre le bleu à %1%", IOActionList::ACTION_SLIDER));
        v.push_back(IOActionList("up_blue %1", "Augmenter le blue", "Augmenter le bleu de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("down_blue %1", "Baisser le blue", "Baisser le bleu de %1%", IOActionList::ACTION_NUMBER));
    }
    else if (params["gui_type"] == "shutter")
    {
        v.push_back(IOActionList("up", "Monter le volet", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("down", "Descendre le volet", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("stop", "Arrêter le volet", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("toggle", "Inverser l'état du volet", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("impulse up %1", "Impulsion sur la montée", "Monter le volet de %1", IOActionList::ACTION_TIME_MS));
        v.push_back(IOActionList("impulse down %1", "Impulsion sur la descente", "Descendre le volet de %1", IOActionList::ACTION_TIME_MS));
    }
    else if (params["gui_type"] == "shutter_smart")
    {
        v.push_back(IOActionList("up", "Monter le volet", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("down", "Descendre le volet", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("stop", "Arrêter le volet", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("toggle", "Inverser l'état du volet", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("set %1", "Mettre le volet à une position", "Mettre le volet à %1%", IOActionList::ACTION_SLIDER));
        v.push_back(IOActionList("up %1", "Monter le volet de X pourcent", "Monter le volet de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("down %1", "Descendre le volet de X pourcent", "Descendre le volet de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("calibrate", "Lancer la calibration", IOActionList::ACTION_SIMPLE));
    }
    else if (params["gui_type"] == "analog_out")
    {
        v.push_back(IOActionList("%1", "Mettre une valeur", "Mettre la valeur %1", IOActionList::ACTION_NUMBER));
    }
    else if (params["gui_type"] == "scenario")
    {
        v.push_back(IOActionList("true", "Lancer le scénario", IOActionList::ACTION_SIMPLE));
        if (params["auto_scenario"] != "")
            v.push_back(IOActionList("false", "Arrêter le scénario", IOActionList::ACTION_SIMPLE));
    }
    else if (params["gui_type"] == "var_string" || params["gui_type"] == "string_out")
    {
        v.push_back(IOActionList("%1", "Mettre une texte", "Mettre le texte '%1'", IOActionList::ACTION_TEXT));
    }
    else if (params["gui_type"] == "var_int")
    {
        v.push_back(IOActionList("%1", "Mettre une valeur", "Mettre la valeur %1", IOActionList::ACTION_NUMBER));
    }
    else if (params["gui_type"] == "var_bool")
    {
        v.push_back(IOActionList("true", "Activer", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("false", "Désactiver", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("toggle", "Inverser l'état", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("impulse loop %1 %1 %1 %1", "Faire clignoter l'état", IOActionList::ACTION_TIME_MS));
        v.push_back(IOActionList("impulse %1", "Activer pendant X secondes", "Activer pendant %1", IOActionList::ACTION_TIME_MS));
    }
    else if (params["gui_type"] == "camera_output")
    {
        v.push_back(IOActionList("recall %1", "Déplacer à la position mémorisée", "Déplacer à la position %1", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("save %1", "Mémoriser la position", "Sauvegarder à la position %1", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("move up", "Déplacer vers le haut", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("move down", "Déplacer vers le bas", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("move left", "Déplacer vers la gauche", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("move right", "Déplacer vers la droite", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("move home", "Déplacer à la position initiale", IOActionList::ACTION_SIMPLE));
    }
    else if (params["gui_type"] == "audio_output")
    {
        v.push_back(IOActionList("play", "Mettre en lecture", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("pause", "Mettre en pause", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("stop", "Arrêter la lecture", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("next", "Piste suivante", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("previous", "Piste précédente", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("volume set %1", "Changer le volume", "Changer le volume à %1%", IOActionList::ACTION_SLIDER));
        v.push_back(IOActionList("volume up %1", "Monter le volume de X pourcent", "Monter le volume de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("volume down %1", "Baisser le volume de X pourcent", "Descendre le volume de %1%", IOActionList::ACTION_NUMBER));
        v.push_back(IOActionList("power on", "Allumer la zone de musique", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("power off", "Eteindre la zone de musique", IOActionList::ACTION_SIMPLE));
        v.push_back(IOActionList("sleep %1", "Eteindre la zone dans X secondes (sleep mode)", "Eteindre la zone dans %1% secondes (sleep mode)", IOActionList::ACTION_NUMBER));
        //TODO: playlist/song selection and playing here
    }

    return v;
}

IOActionList IOBase::getActionFromState()
{
    IOActionList ac;

    if (params["gui_type"] == "light")
    {
        if (params["state"] == "true")
            ac = IOActionList("true", "Allumer la lumière", IOActionList::ACTION_SIMPLE);
        else
            ac = IOActionList("false", "Eteindre la lumière", IOActionList::ACTION_SIMPLE);
    }
    else if (params["gui_type"] == "light_dimmer")
    {
        if (params["state"] == "true")
            ac = IOActionList("true", "Allumer la lumière", IOActionList::ACTION_SIMPLE);
        else if (params["state"] == "false")
            ac = IOActionList("false", "Eteindre la lumière", IOActionList::ACTION_SIMPLE);
        else
            ac = IOActionList("set %1", "Mettre la lumière à X pourcent", "Mettre la lumière à %1%", IOActionList::ACTION_SLIDER);

        ac.dvalue = getDaliValueFromState();
    }
    else if (params["gui_type"] == "light_rgb")
    {
        if (params["state"] == "true")
            ac = IOActionList("true", "Allumer la lumière", IOActionList::ACTION_SIMPLE);
        else if (params["state"] == "false")
            ac = IOActionList("false", "Eteindre la lumière", IOActionList::ACTION_SIMPLE);
        else
            ac = IOActionList("set %1", "Choisir une couleur", "Mettre la couleur", IOActionList::ACTION_COLOR);

        getRGBValueFromState(ac.red, ac.green, ac.blue);
    }
    else if (params["gui_type"] == "shutter")
    {
        ac = IOActionList("up", "Monter le volet", IOActionList::ACTION_SIMPLE);
    }
    else if (params["gui_type"] == "shutter_smart")
    {
        ac = IOActionList("up", "Monter le volet", IOActionList::ACTION_SIMPLE);
    }
    else if (params["gui_type"] == "analog_out")
    {
        ac = IOActionList("%1", "Mettre une valeur", "Mettre la valeur %1", IOActionList::ACTION_NUMBER);
        ac.dvalue = 0.0;
    }
    else if (params["gui_type"] == "scenario")
    {
        ac = IOActionList("true", "Lancer le scénario", IOActionList::ACTION_SIMPLE);
    }
    else if (params["gui_type"] == "var_string" || params["gui_type"] == "string_out")
    {
        ac = IOActionList("%1", "Mettre une texte", "Mettre le texte '%1'", IOActionList::ACTION_TEXT);
        ac.svalue = "Un Texte";
    }
    else if (params["gui_type"] == "var_int")
    {
        ac = IOActionList("%1", "Mettre une valeur", "Mettre la valeur %1", IOActionList::ACTION_NUMBER);
        ac.dvalue = 0.0;
    }
    else if (params["gui_type"] == "var_bool")
    {
        ac = IOActionList("true", "Activer", IOActionList::ACTION_SIMPLE);
    }
    else if (params["gui_type"] == "camera_output")
    {
        ac = IOActionList("recall %1", "Déplacer à la position mémorisée", "Déplacer à la position %1", IOActionList::ACTION_NUMBER);
        ac.dvalue = 0.0;
    }
    else if (params["gui_type"] == "audio_output")
    {
        ac = IOActionList("play", "Mettre en lecture", IOActionList::ACTION_SIMPLE);
    }

    return ac;
}

IOActionList IOBase::getActionListFromAction(string action)
{
    IOActionList ac;
    vector<string> tokens;

    split(action, tokens);
    if (tokens.size() < 1) return ac;

    if (params["gui_type"] == "light")
    {
        if (tokens[0] == "true") ac = IOActionList("true", "Allumer la lumière", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "false") ac = IOActionList("false", "Eteindre la lumière", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "toggle") ac = IOActionList("toggle", "Inverser l'état de la lumière", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "impulse" && tokens[1] == "loop") ac = IOActionList("impulse loop %1 %1 %1 %1", "Faire clignoter la lumière", IOActionList::ACTION_TIME_MS);
        else if (tokens[0] == "impulse") ac = IOActionList("impulse %1", "Allumer la lumière pendant X secondes", "Allumer la lumière pendant %1", IOActionList::ACTION_TIME_MS);

        if (tokens.size() > 1)
            from_string(tokens[tokens.size() - 1], ac.dvalue);
    }
    else if (params["gui_type"] == "light_dimmer")
    {
        if (tokens[0] == "true") ac = IOActionList("true", "Allumer la lumière", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "false") ac = IOActionList("false", "Eteindre la lumière", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "toggle") ac = IOActionList("toggle", "Inverser l'état de la lumière", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "set" && tokens[1] == "off") ac = IOActionList("set off %1", "Mettre la lumière sans allumer à X pourcent", "Mettre la lumière à %1% sans allumer", IOActionList::ACTION_SLIDER);
        else if (tokens[0] == "set") ac = IOActionList("set %1", "Mettre la lumière à X pourcent", "Mettre la lumière à %1%", IOActionList::ACTION_SLIDER);
        else if (tokens[0] == "up") ac = IOActionList("up %1", "Augmenter l'intensité de X pourcent", "Augmenter l'intensité de %1%", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "down") ac = IOActionList("down %1", "Baisser l'intensité de X pourcent", "Baisser l'intensité de %1%", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "impulse" && tokens[1] == "loop") ac = IOActionList("impulse loop %1 %1 %1 %1", "Faire clignoter la lumière", IOActionList::ACTION_TIME_MS);
        else if (tokens[0] == "impulse") ac = IOActionList("impulse %1", "Allumer la lumière pendant X secondes", "Allumer la lumière pendant %1", IOActionList::ACTION_TIME_MS);

        if (tokens.size() > 1)
            from_string(tokens[tokens.size() - 1], ac.dvalue);
    }
    else if (params["gui_type"] == "light_rgb")
    {
        if (tokens[0] == "true") ac = IOActionList("true", "Allumer la lumière", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "false") ac = IOActionList("false", "Eteindre la lumière", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "toggle") ac = IOActionList("toggle", "Inverser l'état de la lumière", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "set") ac = IOActionList("set %1", "Choisir une couleur", "Mettre la couleur", IOActionList::ACTION_COLOR);
        else if (tokens[0] == "set_red") ac = IOActionList("set_red %1", "Choisir l'intensité du rouge", "Mettre le rouge à %1%", IOActionList::ACTION_SLIDER);
        else if (tokens[0] == "up_red") ac = IOActionList("up_red %1", "Augmenter le rouge de X pourcent", "Augmenter le rouge de %1%", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "down_red") ac = IOActionList("down_red %1", "Baisser le rouge de X pourcent", "Baisser le rouge de %1%", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "set_green") ac = IOActionList("set_green %1", "Choisir l'intensité du vert", "Mettre le vert à %1%", IOActionList::ACTION_SLIDER);
        else if (tokens[0] == "up_green") ac = IOActionList("up_green %1", "Augmenter le vert", "Augmenter le vert de %1%", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "down_green") ac = IOActionList("down_green %1", "Baisser le vert", "Baisser le vert de %1%", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "set_blue") ac = IOActionList("set_blue %1", "Choisir l'intensité du bleu", "Mettre le bleu à %1%", IOActionList::ACTION_SLIDER);
        else if (tokens[0] == "up_blue") ac = IOActionList("up_blue %1", "Augmenter le blue", "Augmenter le bleu de %1%", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "down_blue") ac = IOActionList("down_blue %1", "Baisser le blue", "Baisser le bleu de %1%", IOActionList::ACTION_NUMBER);

        if (tokens.size() > 1)
            from_string(tokens[tokens.size() - 1], ac.dvalue);
    }
    else if (params["gui_type"] == "shutter")
    {
        if (tokens[0] == "up") ac = IOActionList("up", "Monter le volet", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "down") ac = IOActionList("down", "Descendre le volet", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "stop") ac = IOActionList("stop", "Arrêter le volet", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "toggle") ac = IOActionList("toggle", "Inverser l'état du volet", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "impulse" && tokens[1] == "up") ac = IOActionList("impulse up %1", "Impulsion sur la montée", "Monter le volet de %1", IOActionList::ACTION_TIME_MS);
        else if (tokens[0] == "impulse" && tokens[1] == "down") ac = IOActionList("impulse down %1", "Impulsion sur la descente", "Descendre le volet de %1", IOActionList::ACTION_TIME_MS);

        if (tokens.size() > 1)
            from_string(tokens[tokens.size() - 1], ac.dvalue);
    }
    else if (params["gui_type"] == "shutter_smart")
    {
        if (tokens[0] == "up") ac = IOActionList("up", "Monter le volet", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "down") ac = IOActionList("down", "Descendre le volet", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "stop") ac = IOActionList("stop", "Arrêter le volet", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "toggle") ac = IOActionList("toggle", "Inverser l'état du volet", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "set") ac = IOActionList("set %1", "Mettre le volet à une position", "Mettre le volet à %1%", IOActionList::ACTION_SLIDER);
        else if (tokens[0] == "up") ac = IOActionList("up %1", "Monter le volet de X pourcent", "Monter le volet de %1%", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "down") ac = IOActionList("down %1", "Descendre le volet de X pourcent", "Descendre le volet de %1%", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "calibrate") ac = IOActionList("calibrate", "Lancer la calibration", IOActionList::ACTION_SIMPLE);

        if (tokens.size() > 1)
            from_string(tokens[tokens.size() - 1], ac.dvalue);
    }
    else if (params["gui_type"] == "analog_out")
    {
        ac = IOActionList("%1", "Mettre une valeur", "Mettre la valeur %1", IOActionList::ACTION_NUMBER);
        from_string(tokens[0], ac.dvalue);
    }
    else if (params["gui_type"] == "scenario")
    {
        if (tokens[0] == "true") ac = IOActionList("true", "Lancer le scénario", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "false") ac = IOActionList("false", "Arrêter le scénario", IOActionList::ACTION_SIMPLE);
    }
    else if (params["gui_type"] == "var_string" || params["gui_type"] == "var_string")
    {
        ac = IOActionList("%1", "Mettre une texte", "Mettre le texte '%1'", IOActionList::ACTION_TEXT);
        ac.svalue = action;
    }
    else if (params["gui_type"] == "var_int")
    {
        ac = IOActionList("%1", "Mettre une valeur", "Mettre la valeur %1", IOActionList::ACTION_NUMBER);
        from_string(tokens[0], ac.dvalue);
    }
    else if (params["gui_type"] == "var_bool")
    {
        if (tokens[0] == "true") ac = IOActionList("true", "Activer", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "false") ac = IOActionList("false", "Désactiver", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "toggle") ac = IOActionList("toggle", "Inverser l'état", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "impulse" && tokens[1] == "loop") ac = IOActionList("impulse loop %1 %1 %1 %1", "Faire clignoter l'état", IOActionList::ACTION_TIME_MS);
        else if (tokens[0] == "impulse") ac = IOActionList("impulse %1", "Activer pendant X secondes", "Activer pendant %1", IOActionList::ACTION_TIME_MS);
    }
    else if (params["gui_type"] == "camera_output")
    {
        if (tokens[0] == "recall")
        {
            ac = IOActionList("recall %1", "Déplacer à la position mémorisée", "Déplacer à la position %1", IOActionList::ACTION_NUMBER);
            from_string(tokens[tokens.size() - 1], ac.dvalue);
        }
        else if (tokens[0] == "save") ac = IOActionList("save %1", "Mémoriser la position", "Sauvegarder à la position %1", IOActionList::ACTION_NUMBER);
        else if (tokens[0] == "move")
        {
            if (tokens[1] == "up") ac = IOActionList("move up", "Déplacer vers le haut", IOActionList::ACTION_SIMPLE);
            else if (tokens[1] == "down") ac = IOActionList("move up", "Déplacer vers le bas", IOActionList::ACTION_SIMPLE);
            else if (tokens[1] == "left") ac = IOActionList("move left", "Déplacer vers la gauche", IOActionList::ACTION_SIMPLE);
            else if (tokens[1] == "right") ac = IOActionList("move right", "Déplacer vers la droite", IOActionList::ACTION_SIMPLE);
            else if (tokens[1] == "home") ac = IOActionList("move home", "Déplacer à la position initiale", IOActionList::ACTION_SIMPLE);
        }
    }
    else if (params["gui_type"] == "audio_output")
    {
        if (tokens[0] == "play") ac = IOActionList("play", "Mettre en lecture", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "pause") ac = IOActionList("pause", "Mettre en pause", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "stop") ac = IOActionList("stop", "Arrêter la lecture", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "next") ac = IOActionList("next", "Piste suivante", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "previous") ac = IOActionList("previous", "Piste précédente", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "volume")
        {
            if (tokens[1] == "set") ac = IOActionList("volume set %1", "Changer le volume", "Changer le volume à %1%", IOActionList::ACTION_SLIDER);
            else if (tokens[1] == "up") ac = IOActionList("volume up %1", "Monter le volume de X pourcent", "Monter le volume de %1%", IOActionList::ACTION_NUMBER);
            else if (tokens[1] == "down") ac = IOActionList("volume down %1", "Baisser le volume de X pourcent", "Descendre le volume de %1%", IOActionList::ACTION_NUMBER);
            from_string(tokens[tokens.size() - 1], ac.dvalue);
        }
        else if (tokens[0] == "power" && tokens[1] == "on") ac = IOActionList("power on", "Allumer la zone de musique", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "power" && tokens[1] == "off") ac = IOActionList("power off", "Eteindre la zone de musique", IOActionList::ACTION_SIMPLE);
        else if (tokens[0] == "sleep")
        {
            ac = IOActionList("sleep %1", "Eteindre la zone dans X secondes (sleep mode)", "Eteindre la zone dans %1% secondes (sleep mode)", IOActionList::ACTION_NUMBER);
            from_string(tokens[tokens.size() - 1], ac.dvalue);
        }
        //TODO: playlist/song selection and playing here
    }

    return ac;
}

string IOActionList::getComputedTitle(IOBase *io)
{
    string t = title_computed;

    if (type == ACTION_SLIDER ||
        type == ACTION_NUMBER)
        Utils::replace_str(t, "%1", Utils::to_string(dvalue));

    if (type == ACTION_TEXT)
        Utils::replace_str(t, "%1", Utils::to_string(svalue));

    if (type == ACTION_TIME_MS)
        Utils::replace_str(t, "%1", Utils::time2string(dvalue / 1000, (long)dvalue % 1000));

    return t;
}

string IOActionList::getComputedAction(IOBase *io)
{
    string ac = action;

    if (type == ACTION_SIMPLE)
        return ac;

    if (type == ACTION_SLIDER ||
        type == ACTION_NUMBER ||
        type == ACTION_TIME_MS)
        Utils::replace_str(ac, "%1", Utils::to_string(dvalue));

    if (type == ACTION_TEXT)
        Utils::replace_str(ac, "%1", Utils::to_string(svalue));

    if (type == ACTION_COLOR)
        Utils::replace_str(ac, "%1", Utils::to_string(io->computeStateFromRGBValue(red, green, blue)));

    return ac;
}

void IOBase::loadPlage()
{
    range_infos.range_monday.clear();
    range_infos.range_tuesday.clear();
    range_infos.range_wednesday.clear();
    range_infos.range_thursday.clear();
    range_infos.range_friday.clear();
    range_infos.range_saturday.clear();
    range_infos.range_sunday.clear();

    if (params["gui_type"] != "time_range")
    {
        cErrorDom("network") << params["id"]
                             << " is not of type time_range, but " << params["gui_type"]
                             << " instead.";
        return;
    }

    string cmd = "input " + params["id"] + " plage get";
    connection->SendCommand(cmd, sigc::mem_fun(*this, &IOBase::loadPlage_cb));
}

void IOBase::loadPlage_cb(bool success, vector<string> result, void *data)
{
    if (!success) return;

    for (uint i = 3;i < result.size();i++)
    {
        vector<string> splitter;
        split(result[i], splitter, ":", 11);

        TimeRange tr;

        tr.shour = splitter[1];
        tr.smin = splitter[2];
        tr.ssec = splitter[3];
        from_string(splitter[4], tr.start_type);
        from_string(splitter[5], tr.start_offset);
        if (tr.start_offset < 0) tr.start_offset = -1;
        if (tr.start_offset >= 0) tr.start_offset = 1;

        tr.ehour = splitter[6];
        tr.emin = splitter[7];
        tr.esec = splitter[8];
        from_string(splitter[9], tr.end_type);
        from_string(splitter[10], tr.end_offset);
        if (tr.end_offset < 0) tr.end_offset = -1;
        if (tr.end_offset >= 0) tr.end_offset = 1;

        int day;
        from_string(splitter[0], day);
        switch(day)
        {
        case 1: range_infos.range_monday.push_back(tr); break;
        case 2: range_infos.range_tuesday.push_back(tr); break;
        case 3: range_infos.range_wednesday.push_back(tr); break;
        case 4: range_infos.range_thursday.push_back(tr); break;
        case 5: range_infos.range_friday.push_back(tr); break;
        case 6: range_infos.range_saturday.push_back(tr); break;
        case 7: range_infos.range_sunday.push_back(tr); break;
        default:
            cErrorDom("network") << params["id"]
                                 << " unknown range day. Debug infos: " << result[i];
            break;
        }
    }

    //Load months info
    string cmd = "input " + params["id"] + " plage months get";
    connection->SendCommand(cmd, sigc::mem_fun(*this, &IOBase::loadPlageMonths_cb));
}

void IOBase::loadPlageMonths_cb(bool success, vector<string> result, void *data)
{
    if (!success) return;

    if (result.size() < 5)
    {
        cErrorDom("network") << params["id"]
                             << "error reading months!";
        io_changed.emit(); //io has changed

        return;
    }

    string m = result[4];
    //reverse to have a left to right months representation
    std::reverse(m.begin(), m.end());

    try
    {
        bitset<12> mset(m);
        range_infos.range_months = mset;
    }
    catch(...)
    {
        cErrorDom("network") << params["id"]
                             << " wrong parameters for months: " << m;
    }

    cDebugDom("network") << params["id"]
                         << " TimeRange reloaded successfully";

    io_changed.emit(); //io has changed
}
