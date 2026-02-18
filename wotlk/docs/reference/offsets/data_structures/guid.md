# GUID Format — WoW 3.3.5a (build 12340)

> 64-bit globally unique identifier for all game objects

## Structure

```
[64-bit GUID]
  Bits 63-52: High type (ObjectType)
  Bits 51-48: Subtype / flags
  Bits 47-0:  Entry / counter
```

## Object Types

| Value | Type | Description |
|-------|------|-------------|
| 0x0000 | TYPEID_OBJECT | Base object |
| 0x0001 | TYPEID_ITEM | Items in inventory |
| 0x0002 | TYPEID_CONTAINER | Bags |
| 0x0003 | TYPEID_UNIT | NPCs, creatures |
| 0x0004 | TYPEID_PLAYER | Player characters |
| 0x0005 | TYPEID_GAMEOBJECT | World objects (chests, doors, etc.) |
| 0x0006 | TYPEID_DYNAMICOBJECT | Dynamic effects |
| 0x0007 | TYPEID_CORPSE | Player corpses |

## High GUID Values

| Value | Meaning |
|-------|---------|
| 0x0000 | Player |
| 0x4000 | Item |
| 0xF110 | Unit (creature) |
| 0xF120 | Pet |
| 0xF130 | Vehicle |
| 0xF140 | GameObject (spawned) |
| 0xF150 | DynamicObject |
| 0xF160 | Corpse |
| 0x1FC0 | GameObject (transport) |
