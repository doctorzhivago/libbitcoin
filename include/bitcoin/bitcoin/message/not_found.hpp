/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef LIBBITCOIN_MESSAGE_NOT_FOUND_HPP
#define LIBBITCOIN_MESSAGE_NOT_FOUND_HPP

#include <istream>
#include <string>
#include <bitcoin/bitcoin/define.hpp>
#include <bitcoin/bitcoin/message/inventory_vector.hpp>
#include <bitcoin/bitcoin/message/inventory.hpp>
#include <bitcoin/bitcoin/utility/data.hpp>

namespace libbitcoin {
namespace message {

class BC_API not_found
  : public inventory
{
public:
    static not_found factory_from_data(const data_chunk& data);
    static not_found factory_from_data(std::istream& stream);
    static not_found factory_from_data(reader& source);

    not_found();
    not_found(const inventory_vector::list& elements);

    static const std::string satoshi_command;
};

} // namspace message
} // namspace libbitcoin

#endif
