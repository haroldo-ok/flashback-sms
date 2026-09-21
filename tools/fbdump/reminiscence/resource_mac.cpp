
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "fs.h"
#include "resource_mac.h"
#include "util.h"

const char *ResourceMac::FILENAME1 = "Flashback.bin";
const char *ResourceMac::FILENAME2 = "Flashback.rsrc";

ResourceMac::ResourceMac(const char *filePath, FileSystem *fs)
	: _dataOffset(0), _types(0), _entries(0),
		_animIndex(-1), _condIndex(-1), _objdIndex(-1), _lmapIndex(-1), _pmovIndex(-1), _polyIndex(-1), _ppssIndex(-1), _sndIndex(-1), _strsIndex(-1), _versIndex(-1) {
	memset(&_map, 0, sizeof(_map));
	_f.open(filePath, "rb", fs);
}

ResourceMac::~ResourceMac() {
	if (_entries) {
		for (int i = 0; i < _map.typesCount; ++i) {
			free(_entries[i]);
		}
		free(_entries);
	}
	free(_types);
}

void ResourceMac::load() {
	const uint32_t sig = _f.readUint32BE();
	if (sig == 0x00051607) { // AppleDouble
		// info("Load Macintosh data from AppleDouble");
		_f.seek(24);
		const int count = _f.readUint16BE();
		for (int i = 0; i < count; ++i) {
			const int id = _f.readUint32BE();
			const int offset = _f.readUint32BE();
			const int length = _f.readUint32BE();
			if (id == 2) { // resource fork
				loadResourceFork(offset, length);
				break;
			}
		}
	} else { // MacBinary
		// info("Load Macintosh data from MacBinary");
		_f.seek(83);
		const uint32_t dataSize = _f.readUint32BE();
		const uint32_t resourceOffset = 128 + ((dataSize + 127) & ~127);
		loadResourceFork(resourceOffset, dataSize);
	}
}

void ResourceMac::loadResourceFork(uint32_t resourceOffset, uint32_t dataSize) {
	_f.seek(resourceOffset);
	_dataOffset = resourceOffset + _f.readUint32BE();
	const uint32_t mapOffset = resourceOffset + _f.readUint32BE();

	_f.seek(mapOffset + 22);
	_f.readUint16BE();
	_map.typesOffset = _f.readUint16BE();
	_map.namesOffset = _f.readUint16BE();
	_map.typesCount = _f.readUint16BE() + 1;

	_f.seek(mapOffset + _map.typesOffset + 2);
	_types = (ResourceMacType *)calloc(_map.typesCount, sizeof(ResourceMacType));
	for (int i = 0; i < _map.typesCount; ++i) {
		_f.read(_types[i].id, 4);
		_types[i].count = _f.readUint16BE() + 1;
		_types[i].startOffset = _f.readUint16BE();
		switch (READ_BE_UINT32(_types[i].id)) {
		case 0x414e494d: // 'ANIM'
			_animIndex = i;
			break;
		case 0x434f4e44: // 'COND'
			_condIndex = i;
			break;
		case 0x4f424a44: // 'OBJD'
			_objdIndex = i;
			break;
		case 0x4c4d4150: // 'LMAP'
			_lmapIndex = i;
			break;
		case 0x504d4f56: // 'PMOV'
			_pmovIndex = i;
			break;
		case 0x504f4c59: // 'POLY'
			_polyIndex = i;
			break;
		case 0x50505353: // 'PPSS'
			_ppssIndex = i;
			break;
		case 0x736e6420: // 'snd '
			_sndIndex = i;
			break;
		case 0x53545253: // 'strs'
			_strsIndex = i;
			break;
		case 0x76657273: // 'vers'
			_versIndex = i;
			break;
		}
	}
	_entries = (ResourceMacEntry **)calloc(_map.typesCount, sizeof(ResourceMacEntry *));
	for (int i = 0; i < _map.typesCount; ++i) {
		_f.seek(mapOffset + _map.typesOffset + _types[i].startOffset);
		_entries[i] = (ResourceMacEntry *)calloc(_types[i].count, sizeof(ResourceMacEntry));
		for (int j = 0; j < _types[i].count; ++j) {
			_entries[i][j].id = _f.readUint16BE();
			_entries[i][j].nameOffset = _f.readUint16BE();
			_entries[i][j].dataOffset = _f.readUint32BE() & 0x00FFFFFF;
			_f.readUint32BE();
		}
		for (int j = 0; j < _types[i].count; ++j) {
			_entries[i][j].name[0] = '\0';
			if (_entries[i][j].nameOffset != 0xFFFF) {
				_f.seek(mapOffset + _map.namesOffset + _entries[i][j].nameOffset);
				const int len = _f.readByte();
				assert(len < kResourceMacEntryNameLength - 1);
				_f.read(_entries[i][j].name, len);
				_entries[i][j].name[len] = '\0';
			}
		}
	}
}

const ResourceMacEntry *ResourceMac::findEntry(const char *name, int type) const {
	if (type < 0) {
		for (type = 0; type < _map.typesCount; ++type) {
			for (int i = 0; i < _types[type].count; ++i) {
				if (strcmp(name, _entries[type][i].name) == 0) {
					// fprintf(stdout, "Found name '%s' sig 0x%x %c%c%c%c\n", name, READ_BE_UINT32(_types[type].id), _types[type].id[0], _types[type].id[1], _types[type].id[2], _types[type].id[3]);
					return &_entries[type][i];
				}
			}
		}
	} else {
		assert(type < _map.typesCount);
		for (int i = 0; i < _types[type].count; ++i) {
			if (strcmp(name, _entries[type][i].name) == 0) {
				return &_entries[type][i];
			}
		}
	}
	return 0;
}

const ResourceMacEntry *ResourceMac::getEntry(int type, int num) const {
	assert(type >= 0 && type < _map.typesCount);
	assert(num >= 0 && _types[type].count);
	return &_entries[type][num];
}
