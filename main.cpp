#include <cinttypes>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "salticidae/event.h"
#include "salticidae/network.h"


using salticidae::generic_bind;
using salticidae::htole;
using salticidae::letoh;
using salticidae::static_pointer_cast;
using salticidae::ConnPool;
using salticidae::DataStream;
using salticidae::EventContext;
using salticidae::NetAddr;
using salticidae::PeerId;
using salticidae::PeerNetwork;
using salticidae::SigEvent;
using salticidae::TimerEvent;
using std::forward;
using std::function;
using std::invoke;
using std::make_pair;
using std::make_unique;
using std::map;
using std::pair;
using std::placeholders::_1;
using std::placeholders::_2;
using std::string;
using std::string_view;
using std::to_string;
using std::unique_ptr;
using std::vector;


static const string command = string(32, 0);
static constexpr uint32_t batch_size = 32000;


/**** MESSAGE TYPES ****/
struct MsgCommand {
	static constexpr uint8_t opcode = 0;
	DataStream serialized;

	MsgCommand() {
		serialized << command;
	}

	MsgCommand(DataStream &&s) {
		s.get_data_inplace(command.size());
	}
};

struct MsgAck {
	static constexpr uint8_t opcode = 1;
	DataStream serialized;
	uint32_t round;

	MsgAck(uint32_t round) {
		serialized << htole(round);
	}

	MsgAck(DataStream &&s) {
		s >> round;
		round = letoh(round);
	}
};


/**** SIMULATED HOTSTUFF ****/
class BaseNet : public PeerNetwork<uint8_t> {
	TimerEvent _gen;							// Timer
	double _speed = 0;							// Timer value
	size_t _rate = 0;							// Number of commands send each time the timer reaches 0
	function<void(MsgCommand &&)> _req_cb;


 public:
	BaseNet(const EventContext &ec, const BaseNet::Config &config)
		: PeerNetwork(ec, config)
		, _gen(ec, generic_bind(&BaseNet::_on_client_request, this,_1)) {
	}

	template<typename Func>
	void reg_client_handler(Func &&cb) {
		_req_cb = forward<Func>(cb);
	}

	void _on_client_request(TimerEvent &) {
		size_t i;
		for (i = 0; i < _rate; i++)
			invoke(_req_cb, MsgCommand());
		_gen.add(_speed);
	}

	void set_client_rate(double ms, size_t ncmd) {
		size_t oldrate = _rate;

		_rate = ncmd;
		_speed = ms;

		if (oldrate == 0)
			_gen.add(ms);
	}
};

class FakeHotstuff : public BaseNet {
	vector<PeerId> _peers;

	size_t _online = 0;
	uint32_t _round = 0;
	size_t _acked = 0;
	function<void(uint32_t)> _round_cb;

	/*
	 * Generates new round of client requests
	 */
	void _new_round() {
		fprintf(stderr, "NEW ROUND\n");
		_round += 1;
		_acked = 0;

		invoke(_round_cb, _round);
	}


 public:
	FakeHotstuff(const EventContext &ec,
		     const FakeHotstuff::Config &config)
		: BaseNet(ec, config) {
		reg_handler(generic_bind(&FakeHotstuff::on_receive_ack,
					 this, _1,_2));
	}

	auto add_peer(const PeerId &peer) {
		_peers.push_back(peer);
		return PeerNetwork::add_peer(peer);
	}

	const vector<PeerId> &peers() const {
		return _peers;
	}

	void ack_batch(uint32_t round, const BaseNet::conn_t &conn) {
		send_msg(MsgAck(round), conn);
	}

	void on_receive_ack(MsgAck &&msg, const BaseNet::conn_t &) {
		if (msg.round != _round)
			return;

		_acked += 1;

		if (_acked == _peers.size())
			_new_round();
	}

	template<typename Func>
	void reg_round_handler(Func &&cb) {
		_round_cb = forward<Func>(cb);
	}

	void start_rounds() {
		if (_round > 0)
			return;
		_new_round();
	}
};


/**** CLASSICAL VERSION ****/
class ClassicHotstuff : public FakeHotstuff {
	size_t _mempool = 0;
	size_t _proposed = 0;


 public:	
	struct MsgBatch {
		static constexpr uint8_t opcode = 100;
		DataStream serialized;
		uint32_t round;

		MsgBatch(uint32_t round, uint32_t ncmd) {
			uint32_t i;

			serialized << htole(round);
			serialized << htole(ncmd);

			for (i = 0; i < ncmd; i++)
				serialized << command;
		}

		MsgBatch(DataStream &&s) {
			uint32_t i, ncmd;

			s >> round;
			round = letoh(round);

			s >> ncmd;
			ncmd = letoh(ncmd);

			for (i = 0; i < ncmd; i++)
				s.get_data_inplace(command.size());
		}
	};


	ClassicHotstuff(const EventContext &ec, const BaseNet::Config &config)
		: FakeHotstuff(ec, config) {
		reg_round_handler(generic_bind(&ClassicHotstuff::on_round,
					       this, _1));
		reg_handler(generic_bind(&ClassicHotstuff::on_receive_batch,
					 this, _1,_2));
		reg_client_handler(generic_bind(&ClassicHotstuff::on_client,
						this, _1));
	}

	void on_round(uint32_t round) {
		uint32_t n;

		if (_mempool > batch_size)
			n = batch_size;
		else
			n = _mempool;

		_mempool -= n;

		printf("type: classic, round: %u, propose: %lu\n",
		       round, _proposed);

		multicast_msg(MsgBatch(round, n), peers());
		_proposed += n;
	}

	void on_receive_batch(MsgBatch &&msg, const BaseNet::conn_t &conn) {
		ack_batch(msg.round, conn);
	}

	void on_client(MsgCommand &&) {
		_mempool += 1;
	}
};


/**** UNBATCHED VERSION ****/
class FakeHotstuffWithStream : public FakeHotstuff {
	using Subnet = PeerNetwork<uint8_t>;
	Subnet _streamer;
	map<PeerId, PeerId> _convert_peers;

	NetAddr _convert_to_stream(const NetAddr &addr) {
		NetAddr stream_addr(addr);

		stream_addr.port = htons(ntohs(stream_addr.port) + 1000);

		return stream_addr;
	}


 public:
	FakeHotstuffWithStream(const EventContext &ec,
			       const BaseNet::Config &config)
		: FakeHotstuff(ec, config), _streamer(ec, config) {
		_streamer.reg_conn_handler([](auto && ...) { return true; });
	}


	void start() {
		FakeHotstuff::start();
		_streamer.start();
	}

	void listen(const NetAddr &addr) {
		FakeHotstuff::listen(addr);
		_streamer.listen(_convert_to_stream(addr));
	}

	void add_peer(const PeerId &peer) {
		FakeHotstuff::add_peer(peer);
	}

	void set_peer_addr(const PeerId &peer, const NetAddr &addr) {
		NetAddr stream_addr = _convert_to_stream(addr);
		PeerId stream_peer = PeerId(stream_addr);

		_streamer.add_peer(stream_peer);

		_convert_peers[peer] = stream_peer;
		_convert_peers[stream_peer] = peer;

		_streamer.set_peer_addr(stream_peer, stream_addr);
		FakeHotstuff::set_peer_addr(peer, addr);
	}

	void conn_peer(const PeerId &peer) {
		_streamer.conn_peer(_convert_peers[peer]);
		FakeHotstuff::conn_peer(peer);
	}


	template<typename Func>
	void reg_stream_handler(Func &&func) {
		_streamer.reg_handler(forward<Func>(func));
	}


	template<typename Msg>
	void stream_send_msg(Msg &&msg, const PeerId &peer) {
		_streamer.send_msg(forward<Msg>(msg), _convert_peers[peer]);
	}

	template<typename Msg>
	void stream_multicast_msg(Msg &&msg, const vector<PeerId> &peers) {
		vector<PeerId> stream_peers;

		for (const auto &p : peers)
			stream_peers.push_back(_convert_peers[p]);

		_streamer.multicast_msg(forward<Msg>(msg), stream_peers);
	}
};

class UnbatchedHotstuff : public FakeHotstuffWithStream {

	map<PeerId, uint64_t> _sequences;
	uint64_t _sequence = 0;
	size_t _mempool = 0;


 public:
	struct MsgGroup {
		static constexpr uint8_t opcode = 100;
		DataStream serialized;
		uint32_t ncmd;

		MsgGroup(uint32_t ncmd) {
			uint32_t i;

			serialized << htole(ncmd);

			for (i = 0; i < ncmd; i++)
				serialized << command;
		}

		MsgGroup(DataStream &&s) {
			uint32_t i;

			s >> ncmd;
			ncmd = letoh(ncmd);

			for (i = 0; i < ncmd; i++)
				s.get_data_inplace(command.size());
		}
	};

	struct MsgSequence {
		static constexpr uint8_t opcode = 101;
		DataStream serialized;
		uint64_t sequence;

		MsgSequence(uint64_t sequence) {
			serialized << htole(sequence);
		}

		MsgSequence(DataStream &&s) {
			s >> sequence;
			sequence = letoh(sequence);
		}
	};

	struct MsgBatch {
		static constexpr uint8_t opcode = 102;
		DataStream serialized;
		uint32_t round;
		uint64_t ncmd;

		MsgBatch(uint32_t round, uint64_t ncmd) {
			serialized << htole(round);
			serialized << htole(ncmd);
		}

		MsgBatch(DataStream &&s) {
			s >> round;
			round = letoh(round);

			s >> ncmd;
			ncmd = letoh(ncmd);
		}
	};


	UnbatchedHotstuff(const EventContext &ec,
			  const BaseNet::Config &config)
		: FakeHotstuffWithStream(ec, config) {
		reg_round_handler(generic_bind(&UnbatchedHotstuff::on_round,
					       this, _1));
		reg_handler(generic_bind(&UnbatchedHotstuff::on_receive_batch,
					 this, _1,_2));

		reg_client_handler(generic_bind(&UnbatchedHotstuff::on_client,
						this, _1));
		reg_stream_handler
			(generic_bind(&UnbatchedHotstuff::on_receive_group,
				      this, _1,_2));
		reg_stream_handler
			(generic_bind(&UnbatchedHotstuff::on_receive_sequence,
				      this, _1,_2));
	}


	void on_round(uint32_t round) {
		uint64_t n = 0;

		for (const auto &kv : _sequences) {
			if ((n == 0) || (kv.second < n))
				n = kv.second;
		}

		printf("type: unbatch, round: %u, propose: %lu\n",
		       round, n);

		multicast_msg(MsgBatch(round, n), peers());

		_sequence = n;
	}

	void on_receive_batch(MsgBatch &&msg, const BaseNet::conn_t &conn) {
		ack_batch(msg.round, conn);
	}

	void on_receive_group(MsgGroup &&msg, const BaseNet::conn_t &conn) {
		uint64_t after, before = _sequence / 10000;

		_sequence += msg.ncmd;

		after = _sequence / 10000;

		if ((after > before) == 0)
			send_msg(MsgSequence(_sequence), conn);
	}

	void on_receive_sequence(MsgSequence &&msg,
				 const BaseNet::conn_t &conn) {
		_sequences[PeerId(conn->get_addr())] = msg.sequence;
		//         ^^^^^^^^^^^^^^^^^^^^^^^^
		// This is the stream peer id and not the peer we know.
		// In this proof of concept, we don't care.
	}

	void on_client(MsgCommand &&) {
		_mempool += 1;

		if (_mempool < 100)
			return;
		// ^^^^^^^^^^^^^^^^^
		// Reduce TCP send overhead (syscall or headers)

		// stream_multicast_msg(msg, peers());
		// ^^^^^^^^^^^^^^^^^^^^
		// Leads to memory overflow (because it's an async call)

		for (const auto &peer : peers())
			stream_send_msg(MsgGroup(_mempool), peer);

		_mempool = 0;
	}
};


/**** ERROR MESSAGE ****/
static void fatal(const char *pgname, const string &msg) {
	fprintf(stderr, "%s\n", msg.c_str());
	fprintf(stderr, "Usage: %s <'classic'|'unbatch'> <this-node-id> "
		"<ip-nodes...>\n", pgname);
	exit(1);
}


/**** TEMPLATE REPLICA FOR EVERY VERSION ****/
template<typename Net>
static void replica(const vector<string> &addrs, unsigned int id) {
	vector<pair<NetAddr, unique_ptr<Net>>> nodes;
	BaseNet::Config config;
	unique_ptr<Net> node;
	bool started = false;
	EventContext ec;
	size_t i;

	config.ping_period(2);
	config.max_msg_size((uint32_t) -1);

	node = make_unique<Net>(ec, config);
	node->start();
	node->listen(NetAddr(addrs[id]));

	for (i = 0; i < addrs.size(); i++) {
		if (i == id)
			continue;

		PeerId pid{addrs[i]};

		node->add_peer(pid);
		node->set_peer_addr(pid, addrs[i]);
		node->conn_peer(pid);
	}

	SigEvent ev_sigint(ec, [id, &node, &started, &ec](int) {
		if (started || (id != 0)) {
			ec.stop();
		} else {
			node->set_client_rate(0.001, 100);
			node->start_rounds();
			started = true;
		}
	});
	ev_sigint.add(SIGINT);

	ec.dispatch();
}


int main(int argc, const char **argv) {
	vector<string> addrs;
	unsigned int id;
	bool unbatch;
	char *err;
	int i;

	if (argc < 2)
		fatal(argv[0], "missing <'classic'|'unbatch'> operand");

	if (::strcmp(argv[1], "classic") == 0)
		unbatch = false;
	else if (::strcmp(argv[1], "unbatch") == 0)
		unbatch = true;
	else
		fatal(argv[0], "invalid <'classic'|'unbatch'> operand");

	if (argc < 3)
		fatal(argv[0], "missing <this-node-id> operand");

	id = ::strtol(argv[2], &err, 10);
	if (*err != '\0')
		fatal(argv[0], "invalid <this-node-id> operand");

	for (i = 3; i < argc; i++)
		addrs.emplace_back(argv[i]);

	if (addrs.size() <= id)
		fatal(argv[0], "not enough <ip-nodes...> operands");

	if (unbatch)
		replica<UnbatchedHotstuff>(addrs, id);
	else
		replica<ClassicHotstuff>(addrs, id);

	return 0;
}

// localhost + netem +500ms delay - test = 30 sec
// type: classic, round: 24, propose: 704000
// type: unbatch, round: 30, propose: 2238000 (ubatch = 1000)
// type: unbatch, round: 30, propose: 2205900 (ubatch = 300)
