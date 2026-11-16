// Ownership-checked column access for the system implementations (3.2,
// schema.hpp). Inside a system function declare
//
//   constexpr schema::SystemId kSys = schema::SystemId::RunEventLogic;
//   schema::Tables& t = im.tables;
//
// and use SND_W(Voice_lifecycle) / SND_R(Playback_eventRef): a write to a
// column the system does not own, or a read it does not declare, fails to
// compile (D-35).
#ifndef SND_CORE_SRC_ACCESS_HPP
#define SND_CORE_SRC_ACCESS_HPP

#include "snd/internal/schema.hpp"

#define SND_W(col) (::snd::schema::WriteColumn<kSys, ::snd::schema::ColumnId::col>(t))
#define SND_R(col) (::snd::schema::ReadColumn<kSys, ::snd::schema::ColumnId::col>(t))
#define SND_ROWS(table) (::snd::schema::CreatorRows<kSys, ::snd::schema::TableId::table>(t))

#endif  // SND_CORE_SRC_ACCESS_HPP
