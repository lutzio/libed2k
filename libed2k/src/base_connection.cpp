
#include "base_connection.hpp"
#include "session.hpp"
#include "session_impl.hpp"

namespace libed2k
{
    base_connection::base_connection(aux::session_impl& ses):
        m_ses(ses), m_socket(new tcp::socket(ses.m_io_service)),
        m_deadline(ses.m_io_service)
    {
        init();
    }

    base_connection::base_connection(
        aux::session_impl& ses, boost::shared_ptr<tcp::socket> s, 
        const tcp::endpoint& remote):
        m_ses(ses), m_socket(s), m_deadline(ses.m_io_service), m_remote(remote)
    {
        init();
    }

    base_connection::~base_connection()
    {
        DBG("~base_connection");
    }

    void base_connection::init()
    {
        m_deadline.expires_at(boost::posix_time::pos_infin);
        m_write_in_progress = false;
    }

    void base_connection::close()
    {
        DBG("base_connection::close()");
        m_socket->close();
        m_deadline.cancel();
    }

    void base_connection::do_read()
    {
        m_deadline.expires_from_now(
            boost::posix_time::seconds(m_ses.settings().peer_timeout));
        boost::asio::async_read(
            *m_socket, boost::asio::buffer(&m_in_header, header_size),
            boost::bind(&base_connection::on_read_header, self(), _1, _2));
    }

    void base_connection::do_write()
    {
        // send the actual buffer
        if (!m_write_in_progress && !m_send_buffer.empty())
        {
            // check quota here
            int amount_to_send = m_send_buffer.size();

            // set deadline timer
            m_deadline.expires_from_now(
                boost::posix_time::seconds(m_ses.settings().peer_timeout));

            const std::list<boost::asio::const_buffer>& buffers =
                m_send_buffer.build_iovec(amount_to_send);
            boost::asio::async_write(
                *m_socket, buffers, make_write_handler(
                    boost::bind(&base_connection::on_write, self(), _1, _2)));
            m_write_in_progress = true;
        }
    }

    void base_connection::copy_send_buffer(char const* buf, int size)
    {
        int free_space = m_send_buffer.space_in_last_buffer();
        if (free_space > size) free_space = size;
        if (free_space > 0)
        {
            m_send_buffer.append(buf, free_space);
            size -= free_space;
            buf += free_space;
        }
        if (size <= 0) return;

        std::pair<char*, int> buffer = m_ses.allocate_buffer(size);
        if (buffer.first == 0)
        {
            on_error(errors::no_memory);
            return;
        }

        std::memcpy(buffer.first, buf, size);
        m_send_buffer.append_buffer(
            buffer.first, buffer.second, size,
            boost::bind(&aux::session_impl::free_buffer,
                        boost::ref(m_ses), _1, buffer.second));
    }

    void base_connection::on_error(const error_code& error)
    {
        ERR("Error " << error.message());
        close();
    }

    void base_connection::on_timeout(const error_code& e)
    {
    }

    void base_connection::on_read_header(const error_code& error, size_t nSize)
    {
        if (is_closed()) return;

        if (!error)
        {
            // we must download body in any case
            // increase internal buffer size if need
            if (m_in_container.size() < m_in_header.m_size - 1)
            {
                m_in_container.resize(m_in_header.m_size - 1);
            }

            boost::asio::async_read(
                *m_socket,
                boost::asio::buffer(&m_in_container[0], m_in_header.m_size - 1),
                boost::bind(&base_connection::on_read_packet, self(), _1, _2));
        }
        else
        {
            on_error(error);
        }

    }

    void base_connection::on_read_packet(const error_code& error, size_t nSize)
    {
        if (is_closed()) return;

        if (!error)
        {
            //!< search appropriate dispatcher
            handler_map::iterator itr = m_handlers.find(m_in_header.m_type);

            if (itr != m_handlers.end())
            {
                DBG("call normal handler");
                itr->second(error);
            }
            else
            {
                DBG("ignore unhandled packet");
            }

            do_read();
        }
        else
        {
            on_error(error);
        }
    }

    void base_connection::on_write(const error_code& error, size_t nSize)
    {
        if (is_closed()) return;

        if (!error) {
            m_send_buffer.pop_front(nSize);
            m_write_in_progress = false;
            do_write();
        }
        else
            on_error(error);
    }

    void base_connection::check_deadline()
    {
        if (is_closed()) return;

        // Check whether the deadline has passed. We compare the deadline against
        // the current time since a new asynchronous operation may have moved the
        // deadline before this actor had a chance to run.
        if (m_deadline.expires_at() <= dtimer::traits_type::now())
        {
            DBG("base_connection::check_deadline(): deadline timer expired");

            // The deadline has passed. The socket is closed so that any outstanding
            // asynchronous operations are cancelled.
            close();
            // There is no longer an active deadline. The expiry is set to positive
            // infinity so that the actor takes no action until a new deadline is set.
            m_deadline.expires_at(boost::posix_time::pos_infin);
            boost::system::error_code ignored_ec;

            on_timeout(ignored_ec);
        }

        // Put the actor back to sleep.
        m_deadline.async_wait(boost::bind(&base_connection::check_deadline, self()));
    }

    void base_connection::add_handler(proto_type ptype, packet_handler handler)
    {
        m_handlers.insert(make_pair(ptype, handler));
    }

}