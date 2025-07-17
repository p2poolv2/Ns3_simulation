#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/depth_first_search.hpp>
#include <boost/graph/topological_sort.hpp>
#include <boost/graph/visitors.hpp>
#include <boost/property_map/property_map.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <boost/graph/reverse_graph.hpp>
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/csma-module.h"
#include "ns3/mobility-module.h"
#include <unordered_set>
#include <unordered_map>
#include <iomanip>
#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <functional>
#include "ns3/netanim-module.h"
#include "ns3/point-to-point-module.h"
#include <cmath> 
#include <chrono>
#include <algorithm>
#include <random>   
#include "ns3/random-variable-stream.h" 
#include "ns3/ptr.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include "ns3/node-container.h"
#include <chrono>

extern ns3::NodeContainer nodes;
ns3::NodeContainer nodes;

std::string outputDir = "dag_output/";




using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TcpGossip");

class MinerApp;

class TcpGossipApp : public Application {
    private:
        Ptr<Socket> m_socket;
        std::vector<Ipv6Address> m_neighbors;
        
    
        // Connection pool implementation
        class ConnectionPool {
        private:
            // max active connections
            static const uint32_t MAX_ACTIVE_CONNECTIONS = 10;
            
            // Map to track persistent connections to neighbors
            std::map<Ipv6Address, Ptr<Socket>> m_neighborSockets;
            // Track if a socket is considered active
            std::map<Ipv6Address, bool> m_socketActive;
            // Track connection priority (lower = higher priority)
            std::map<Ipv6Address, uint32_t> m_connectionPriority;
            // Weak references to incoming sockets
            std::unordered_set<Ptr<Socket>> m_incomingSockets;
            
            // Last time we exchanged data with this neighbor
            std::map<Ipv6Address, double> m_lastActivity;
            
            // Reference to parent app
            TcpGossipApp* m_app;
            
        public:
            ConnectionPool(TcpGossipApp* app) : m_app(app) {}
            
            void AddNeighbor(Ipv6Address neighbor) {
                if (m_neighborSockets.find(neighbor) == m_neighborSockets.end()) {
                    m_neighborSockets[neighbor] = nullptr;
                    m_socketActive[neighbor] = false;
                    m_connectionPriority[neighbor] = rand() % 100;
                    m_lastActivity[neighbor] = 0.0;
                }
            }
            
            void RemoveNeighbor(Ipv6Address neighbor) {
                auto socketIt = m_neighborSockets.find(neighbor);
                if (socketIt != m_neighborSockets.end()) {
                    if (socketIt->second) {
                        socketIt->second->Close();
                    }
                    m_neighborSockets.erase(socketIt);
                }
                m_socketActive.erase(neighbor);
                m_connectionPriority.erase(neighbor);
                m_lastActivity.erase(neighbor);
            }
            
            void AddIncomingSocket(Ptr<Socket> socket) {
                m_incomingSockets.insert(socket);
            }
            
            void RemoveIncomingSocket(Ptr<Socket> socket) {
                m_incomingSockets.erase(socket);
            }
            
            void UpdateActivity(Ipv6Address neighbor) {
                m_lastActivity[neighbor] = Simulator::Now().GetSeconds();
                m_connectionPriority[neighbor] = m_connectionPriority[neighbor] / 2;
            }
            
            void UpdateActivityFromSocket(Ptr<Socket> socket) {
                for (auto& pair : m_neighborSockets) {
                    if (pair.second == socket) {
                        UpdateActivity(pair.first);
                        break;
                    }
                }
            }
            
            void SetSocket(Ipv6Address neighbor, Ptr<Socket> socket) {
                m_neighborSockets[neighbor] = socket;
            }
            
            void SetSocketActive(Ipv6Address neighbor, bool active) {
                m_socketActive[neighbor] = active;
            }
            
            bool IsActive(Ipv6Address neighbor) const {
                auto it = m_socketActive.find(neighbor);
                return (it != m_socketActive.end() && it->second);
            }
            
            Ptr<Socket> GetSocket(Ipv6Address neighbor) {
                auto it = m_neighborSockets.find(neighbor);
                return (it != m_neighborSockets.end()) ? it->second : nullptr;
            }
            
            uint32_t GetActiveConnectionCount() const {
                uint32_t count = 0;
                for (const auto& pair : m_socketActive) {
                    if (pair.second) count++;
                }
                return count;
            }
            
            std::vector<Ipv6Address> GetPriorityNeighbors() {
                std::vector<std::pair<Ipv6Address, uint32_t>> neighbors;
                
                for (const auto& pair : m_connectionPriority) {
                    neighbors.push_back(std::make_pair(pair.first, pair.second));
                }
                
                std::sort(neighbors.begin(), neighbors.end(), 
                        [](const auto& a, const auto& b) {
                            return a.second < b.second;
                        });
                
                std::vector<Ipv6Address> result;
                for (const auto& pair : neighbors) {
                    result.push_back(pair.first);
                }
                
                return result;
            }
            
            void ManageConnections() {
                uint32_t activeConnections = GetActiveConnectionCount();
                
                if (activeConnections < MAX_ACTIVE_CONNECTIONS && m_neighborSockets.size() > 0) {
                    std::vector<Ipv6Address> priorityNeighbors = GetPriorityNeighbors();
                    
                    for (const auto& neighbor : priorityNeighbors) {
                        if (activeConnections >= MAX_ACTIVE_CONNECTIONS) break;
                        
                        if (!IsActive(neighbor)) {
                            Simulator::Schedule(MilliSeconds(rand() % 100), 
                                              &TcpGossipApp::ConnectToNeighbor, 
                                              m_app, neighbor);
                            
                            activeConnections++;
                        }
                    }
                }
                
                if (activeConnections > MAX_ACTIVE_CONNECTIONS) {
                    std::vector<Ipv6Address> priorityNeighbors = GetPriorityNeighbors();
                    std::reverse(priorityNeighbors.begin(), priorityNeighbors.end());
                    
                    for (const auto& neighbor : priorityNeighbors) {
                        if (activeConnections <= MAX_ACTIVE_CONNECTIONS) break;
                        
                        if (IsActive(neighbor)) {
                            auto socket = GetSocket(neighbor);
                            if (socket) {
                                socket->Close();
                                SetSocket(neighbor, nullptr);
                            }
                            SetSocketActive(neighbor, false);
                            activeConnections--;
                        }
                    }
                }
            }
            
            void CloseAllConnections() {
                for (auto& socketPair : m_neighborSockets) {
                    if (socketPair.second) {
                        socketPair.second->Close();
                    }
                }
                m_neighborSockets.clear();
                m_socketActive.clear();
                
                for (auto& socket : m_incomingSockets) {
                    socket->Close();
                }
                m_incomingSockets.clear();
            }
        };
        
        // Enhanced message manager for blockchain messages
        class MessageManager {
        private:
            std::unordered_set<std::string> m_receivedMessages;
            std::unordered_set<std::string> m_forwardedMessages;
            std::unordered_set<std::string> m_receivedBlocks;
            uint32_t m_receivedBlockCount;
            
            // Track missing blocks that we've requested
            std::unordered_set<std::string> m_requestedBlocks;
            std::map<std::string, double> m_requestedBlockTime;
            std::map<std::string, double> m_pendingBlockRequests;
            // static const double BLOCK_REQUEST_TIMEOUT = 30.0; // 30 seconds timeout
            static constexpr double BLOCK_REQUEST_TIMEOUT = 30.0;

            
        public:
            MessageManager() : m_receivedBlockCount(0) {}
            
            bool IsReceived(const std::string& msg) const {
                return m_receivedMessages.find(msg) != m_receivedMessages.end();
            }
            
            bool IsForwarded(const std::string& msg) const {
                return m_forwardedMessages.find(msg) != m_forwardedMessages.end();
            }
            
            bool IsBlockReceived(const std::string& blockHash) const {
                return m_receivedBlocks.find(blockHash) != m_receivedBlocks.end();
            }
            
            bool IsBlockRequested(const std::string& blockHash) const {
                return m_requestedBlocks.find(blockHash) != m_requestedBlocks.end();
            }
            
            void MarkReceived(const std::string& msg) {
                m_receivedMessages.insert(msg);
            }
            
            void MarkBlockReceived(const std::string& blockHash) {
                if (m_receivedBlocks.find(blockHash) == m_receivedBlocks.end()) {
                    m_receivedBlocks.insert(blockHash);
                    m_receivedBlockCount++;
                }
                // Remove from requested list if it was requested
                m_requestedBlocks.erase(blockHash);
                m_requestedBlockTime.erase(blockHash);
            }
            
            void MarkForwarded(const std::string& msg) {
                m_forwardedMessages.insert(msg);
            }
            
            void MarkBlockRequested(const std::string& blockHash) {
                m_requestedBlocks.insert(blockHash);
                m_requestedBlockTime[blockHash] = Simulator::Now().GetSeconds();
            }
            
            std::vector<std::string> GetTimedOutRequests() {
                std::vector<std::string> timedOut;
                double currentTime = Simulator::Now().GetSeconds();
                
                for (auto it = m_requestedBlockTime.begin(); it != m_requestedBlockTime.end();) {
                    if (currentTime - it->second > BLOCK_REQUEST_TIMEOUT) {
                        timedOut.push_back(it->first);
                        m_requestedBlocks.erase(it->first);
                        it = m_requestedBlockTime.erase(it);
                    } else {
                        ++it;
                    }
                }
                
                return timedOut;
            }
            
            uint32_t GetReceivedBlockCount() const {
                return m_receivedBlockCount;
            }
        };
        
        ConnectionPool m_connectionPool;
        MessageManager m_messageManager;
        
        Ipv6Address m_myAddress;
        uint32_t m_nodeId;
        bool m_isSender;
        
        // Reference to the miner application
        Ptr<MinerApp> m_minerApp;
        
        
        // For connection management
        EventId m_connectionCheckEvent;
        bool m_connectionsEstablished;
        
        // For batched message forwarding with latency control
        EventId m_forwardEvent;
        static const Time FORWARD_INTERVAL;
        
        // For scheduled message sending with latency
        std::queue<std::pair<std::string, std::vector<Ipv6Address>>> m_messageQueue;
        std::map<std::string, EventId> m_scheduledMessages;
    
    public:
        TcpGossipApp(Ipv6Address myAddress) 
            : m_myAddress(myAddress), 
              m_isSender(false),
              m_connectionsEstablished(false),
              m_connectionPool(this),
              m_messageManager() {}
    
        void AddNeighbor(Ipv6Address neighbor) {
            if (neighbor != m_myAddress) {
                m_neighbors.push_back(neighbor);
                m_connectionPool.AddNeighbor(neighbor);
            }
        }

        void GetNeighbors(std::vector<Ipv6Address>& neighbors) const {
            neighbors = m_neighbors;
        }
    
        void RemoveNeighbor(Ipv6Address neighbor) {
            for (auto it = m_neighbors.begin(); it != m_neighbors.end(); ++it) {
                if (*it == neighbor) {
                    m_neighbors.erase(it);
                    m_connectionPool.RemoveNeighbor(neighbor);
                    return;
                }
            }
        }

        void SetMinerApp(Ptr<MinerApp> minerApp) {
            m_minerApp = minerApp;
        }
    
        void PrintNeighbors() const {
            std::cout << "Neighbors of " << m_myAddress << " (Node " << m_nodeId << "):" << std::endl;
            for (const auto& neighbor : m_neighbors) {
                std::cout << "  " << neighbor << std::endl;
            }
        }
        
        void StartApplication() override {
            m_nodeId = GetNode()->GetId();
            
            if (m_nodeId % 50 == 0) {
                PrintNeighbors();
            }
            
            m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
            m_socket->Bind(Inet6SocketAddress(Ipv6Address::GetAny(), 8080));
            m_socket->Listen();
    
            m_socket->SetAcceptCallback(
                MakeCallback(&TcpGossipApp::AcceptConnection, this),
                MakeCallback(&TcpGossipApp::HandleAccept, this)
            );
            
            Time delay = MilliSeconds(100 + (m_nodeId % 1000));
            Simulator::Schedule(delay, &TcpGossipApp::EstablishConnections, this);
            
            m_connectionCheckEvent = Simulator::Schedule(
                Seconds(2.0 + (double)(m_nodeId % 100) / 100.0), 
                &TcpGossipApp::CheckConnections, this);
        }
        
        void StopApplication() override {
            if (m_connectionCheckEvent.IsRunning()) {
                Simulator::Cancel(m_connectionCheckEvent);
            }
            
            if (m_forwardEvent.IsRunning()) {
                Simulator::Cancel(m_forwardEvent);
            }
            
            // Cancel all scheduled message forwarding events
            for (auto& scheduledMsg : m_scheduledMessages) {
                if (scheduledMsg.second.IsRunning()) {
                    Simulator::Cancel(scheduledMsg.second);
                }
            }
            m_scheduledMessages.clear();
            
            m_connectionPool.CloseAllConnections();
            
            if (m_socket) {
                m_socket->Close();
                m_socket = nullptr;
            }
        }
        
        void EstablishConnections() {
            m_connectionPool.ManageConnections();
            m_connectionsEstablished = true;
        }
        
        void ConnectToNeighbor(Ipv6Address neighborAddr) {
            Ptr<Socket> socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
            
            socket->SetConnectCallback(
                MakeCallback(&TcpGossipApp::ConnectionSucceeded, this),
                MakeCallback(&TcpGossipApp::ConnectionFailed, this)
            );
            
            socket->SetRecvCallback(MakeCallback(&TcpGossipApp::ReceiveMessage, this));
            
            m_connectionPool.SetSocket(neighborAddr, socket);
            m_connectionPool.SetSocketActive(neighborAddr, false);
            
            socket->Connect(Inet6SocketAddress(neighborAddr, 8080));
        }
        
        void CheckConnections() {
            m_connectionPool.ManageConnections();
            
            // Clean up timed out block requests
            auto timedOutRequests = m_messageManager.GetTimedOutRequests();
            if (!timedOutRequests.empty()) {
                NS_LOG_WARN("Node " << m_nodeId << " has " << timedOutRequests.size() << " timed out block requests");
            }
            
            double jitter = (double)(rand() % 500) / 1000.0;
            m_connectionCheckEvent = Simulator::Schedule(
                Seconds(5.0 + jitter), 
                &TcpGossipApp::CheckConnections, this);
        }
        
        void SendHeartbeat(Ptr<Socket> socket) {
            std::string heartbeat = "h\n";
            Ptr<Packet> packet = Create<Packet>((uint8_t*)heartbeat.c_str(), heartbeat.size());
            socket->Send(packet);
        }
    
        bool AcceptConnection(Ptr<Socket> socket, const Address &from) {
            return true;  
        }
    
        void HandleAccept(Ptr<Socket> socket, const Address &from) {
            m_connectionPool.AddIncomingSocket(socket);
            socket->SetRecvCallback(MakeCallback(&TcpGossipApp::ReceiveMessage, this));
        }

        void ReceiveMessage(Ptr<Socket> socket) {
            Address from;
            socket->GetPeerName(from);
            
            Ptr<Packet> packet = socket->Recv();
            if (!packet || packet->GetSize() == 0) {
                m_connectionPool.RemoveIncomingSocket(socket);
                
                if (from.IsInvalid() == false) {
                    try {
                        Inet6SocketAddress inet6Addr = Inet6SocketAddress::ConvertFrom(from);
                        Ipv6Address peerAddr = inet6Addr.GetIpv6();
                        m_connectionPool.SetSocketActive(peerAddr, false);
                    } catch (const std::exception& e) {
                        // Address conversion failed, ignore
                    }
                }
                
                return;
            }
            
            if (from.IsInvalid() == false) {
                try {
                    Inet6SocketAddress inet6Addr = Inet6SocketAddress::ConvertFrom(from);
                    Ipv6Address peerAddr = inet6Addr.GetIpv6();
                    m_connectionPool.UpdateActivity(peerAddr);
                    m_connectionPool.SetSocketActive(peerAddr, true);
                } catch (const std::exception& e) {
                    // Address conversion failed, ignore
                }
            } else {
                m_connectionPool.UpdateActivityFromSocket(socket);
            }
            
            uint32_t size = packet->GetSize();
            std::vector<uint8_t> buffer(size);  
            packet->CopyData(buffer.data(), size);
            
            std::string data(buffer.begin(), buffer.end());
            std::istringstream stream(data);
            std::string line;
            
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                
                if (line.empty()) continue;
                
                if (line == "h") {
                    continue;
                }
                
                ProcessReceivedMessage(line);
            }
        }
        
        void HandleMissingBlockRequest(const std::string& message);
        void HandleMissingBlockResponse(const std::string& message);
        void ProcessReceivedMessage(const std::string& message);

        bool IsBlockchainMessage(const std::string& message) {
            size_t pipeCount = std::count(message.begin(), message.end(), '|');
            return pipeCount == 6;
        }

        std::string ExtractBlockHash(const std::string& serializedBlock) {
            if (!IsBlockchainMessage(serializedBlock)) {
                return "";
            }
            
            size_t firstPipe = serializedBlock.find('|');
            if (firstPipe != std::string::npos) {
                return serializedBlock.substr(0, firstPipe);
            }
            
            return "";
        }

        void SendBlockMessage(const std::string& serializedBlock) {
            std::string blockHash = ExtractBlockHash(serializedBlock);
            
            if (!blockHash.empty() && m_messageManager.IsBlockReceived(blockHash)) {
                return;
            }
            
            if (!blockHash.empty()) {
                m_messageManager.MarkBlockReceived(blockHash);
            }
            
            SendMessage(serializedBlock);
        }

        void SendMessage(const std::string& msg) {
            if (m_messageManager.IsForwarded(msg)) {
                return;
            }
            
            m_messageManager.MarkReceived(msg);
            m_messageManager.MarkForwarded(msg);
            
            ForwardMessage(msg);
        }

        // New method to request missing blocks
        void RequestMissingBlocks(const std::vector<std::string>& missingHashes) {
            for (const std::string& hash : missingHashes) {
                if (!m_messageManager.IsBlockRequested(hash)) {
                    // Create block request message
                    std::string requestMsg = "REQUEST_BLOCK|" + hash;
                    
                    m_messageManager.MarkBlockRequested(hash);
                    
                    NS_LOG_INFO("Node " << m_nodeId << " requesting missing block: " << hash);
                    
                    // Send request to all active neighbors
                    ForwardMessage(requestMsg);
                }
            }
        }

        // Modified ForwardMessage to use latency control
        void ForwardMessage(const std::string& msg) {
            std::vector<Ipv6Address> activeNeighbors;
            for (const auto& neighbor : m_neighbors) {
                if (m_connectionPool.IsActive(neighbor)) {
                    activeNeighbors.push_back(neighbor);
                }
            }
            
            if (activeNeighbors.empty()) {
                return;
            }
            
            // Create a unique key for this message to avoid duplicate scheduling
            std::string msgKey = msg + "_" + std::to_string(Simulator::Now().GetSeconds());
            
            // Cancel any existing scheduled event for this message
            auto it = m_scheduledMessages.find(msgKey);
            if (it != m_scheduledMessages.end() && it->second.IsRunning()) {
                Simulator::Cancel(it->second);
            }
            
            // Schedule the message to be sent after FORWARD_INTERVAL
            EventId eventId = Simulator::Schedule(
                FORWARD_INTERVAL,
                &TcpGossipApp::DoForwardMessage,
                this,
                msg,
                activeNeighbors
            );
            
            m_scheduledMessages[msgKey] = eventId;
        }
        
        // New method to actually send the message (called after delay)
        void DoForwardMessage(const std::string& msg, const std::vector<Ipv6Address>& neighbors) {
            std::string msgWithDelimiter = msg + "\n";
            
            for (const auto& neighbor : neighbors) {
                // Double-check if neighbor is still active
                if (!m_connectionPool.IsActive(neighbor)) {
                    continue;
                }
                
                Ptr<Socket> socket = m_connectionPool.GetSocket(neighbor);
                
                if (socket) {
                    Ptr<Packet> packet = Create<Packet>((uint8_t*)msgWithDelimiter.c_str(), msgWithDelimiter.size());
                    int bytes = socket->Send(packet);
                    
                    if (bytes <= 0) {
                        m_connectionPool.SetSocketActive(neighbor, false);
                    }
                }
            }
        }
    
        void ConnectionSucceeded(Ptr<Socket> socket) {
            Address from;
            socket->GetPeerName(from);
            
            try {
                Inet6SocketAddress inet6Addr = Inet6SocketAddress::ConvertFrom(from);
                Ipv6Address peerAddr = inet6Addr.GetIpv6();
                m_connectionPool.SetSocketActive(peerAddr, true);
            } catch (const std::exception& e) {
                // Address conversion failed, ignore
            }
        }
    
        void ConnectionFailed(Ptr<Socket> socket) {
            // Let the connection pool handle reconnection
        }
    
        void SetSender() { m_isSender = true; }
    
        uint32_t GetReceivedBlockCount() const {
            return m_messageManager.GetReceivedBlockCount();
        }
        
        uint32_t GetNodeId() const {
            return m_nodeId;
        }
        
        uint32_t GetConnectedNeighborCount() const {
            return m_connectionPool.GetActiveConnectionCount();
        }
    };
    
    // You can now control the forwarding latency by changing this value
    const Time TcpGossipApp::FORWARD_INTERVAL = MilliSeconds(0);


struct Share {
    std::string blockHash;
    std::string parentHash;
    uint32_t blockNumber;
    uint32_t nodeId;
    double timestamp;
    double receivedTime;
    uint32_t chainWeight;
    uint32_t difficulty;
    std::vector<uint32_t> propagationPath;
    
    // DAG-specific fields
    uint32_t workDone;
    std::vector<std::string> references;  // Additional block references for DAG
    uint32_t cumulativeWork;  // Total work from genesis to this block
    
    Share(const std::string& hash, const std::string& parent, uint32_t number, 
          uint32_t miner, double time, uint32_t diff = 1) 
        : blockHash(hash), parentHash(parent), blockNumber(number), 
          nodeId(miner), timestamp(time), receivedTime(0), 
          chainWeight(0), difficulty(diff), workDone(diff), cumulativeWork(diff) {
        propagationPath.push_back(miner);
    }
    
    Share(const Share& other, double receiveTime) 
        : blockHash(other.blockHash), parentHash(other.parentHash), 
          blockNumber(other.blockNumber), nodeId(other.nodeId), 
          timestamp(other.timestamp), receivedTime(receiveTime),
          chainWeight(other.chainWeight), difficulty(other.difficulty),
          propagationPath(other.propagationPath), workDone(other.workDone),
          references(other.references), cumulativeWork(other.cumulativeWork) {}
    
    void calculateChainWeight(const std::map<std::string, std::shared_ptr<Share>>& blockchain) {
        if (parentHash == "genesis" || parentHash.empty()) {
            chainWeight = difficulty;
            workDone = difficulty;
            cumulativeWork = difficulty;
        } else {
            auto parentIt = blockchain.find(parentHash);
            if (parentIt != blockchain.end()) {
                chainWeight = parentIt->second->chainWeight + difficulty;
                workDone = parentIt->second->workDone + difficulty;
                cumulativeWork = parentIt->second->cumulativeWork + difficulty;
            } else {
                chainWeight = difficulty;
                workDone = difficulty;
                cumulativeWork = difficulty;
            }
        }
    }
    
    std::string serialize() const {
        std::stringstream ss;
        ss << blockHash << "|" << parentHash << "|" << blockNumber << "|" 
           << nodeId << "|" << timestamp << "|" << chainWeight << "|" << difficulty;
        return ss.str();
    }
    
    static std::shared_ptr<Share> deserialize(const std::string& data, double receiveTime) {
        std::stringstream ss(data);
        std::string hash, parent;
        uint32_t number, miner, weight, diff;
        double time;
        
        std::getline(ss, hash, '|');
        std::getline(ss, parent, '|');
        ss >> number; ss.ignore();
        ss >> miner; ss.ignore();
        ss >> time; ss.ignore();
        ss >> weight; ss.ignore();
        ss >> diff;
        
        auto share = std::make_shared<Share>(hash, parent, number, miner, time, diff);
        share->chainWeight = weight;
        share->workDone = weight;
        share->cumulativeWork = weight;
        share->receivedTime = receiveTime;
        return share;
    }
};

// Boost Graph definitions for DAG
struct BlockVertex {
    std::string blockHash;
    std::shared_ptr<Share> share;
    uint32_t cumulativeWork;
    bool isOnMainChain;
    bool isProcessed;
    
    BlockVertex() : cumulativeWork(0), isOnMainChain(false), isProcessed(false) {}
    BlockVertex(const std::string& hash, std::shared_ptr<Share> s) 
        : blockHash(hash), share(s), cumulativeWork(s->cumulativeWork), 
          isOnMainChain(false), isProcessed(false) {}
};

struct BlockEdge {
    std::string edgeType; // "parent", "reference"
    uint32_t weight;
    
    BlockEdge(const std::string& type = "parent", uint32_t w = 1) 
        : edgeType(type), weight(w) {}
};

typedef boost::adjacency_list<
    boost::vecS,           // OutEdgeList
    boost::vecS,           // VertexList  
    boost::bidirectionalS, // Directed graph
    BlockVertex,           // VertexProperties
    BlockEdge              // EdgeProperties
> BlockDAG;

typedef boost::graph_traits<BlockDAG>::vertex_descriptor VertexDescriptor;
typedef boost::graph_traits<BlockDAG>::edge_descriptor EdgeDescriptor;
typedef boost::graph_traits<BlockDAG>::in_edge_iterator InEdgeIterator;
typedef boost::graph_traits<BlockDAG>::out_edge_iterator OutEdgeIterator;

enum class BlockRelation {
    PARENT,
    CHILD,
    SIBLING,
    UNCLE,
    REORG_NEEDED,
    ORPHAN,
    DUPLICATE,
    INVALID,
    REJECTED_UNCLE
};

class BlockchainState {
private:
    // DAG structure using Boost Graph Library
    BlockDAG m_dag;
    
    // Hash to vertex descriptor mapping
    std::map<std::string, VertexDescriptor> m_hashToVertex;
    
    // Traditional blockchain mappings for compatibility
    std::map<std::string, std::shared_ptr<Share>> m_blockchain;
    std::string m_currentHead;
    std::map<uint32_t, std::vector<std::string>> m_blocksByHeight;
    uint32_t m_currentHeight;
    
    // Uncle management
    uint32_t m_maxUnclesPerHeight;
    std::map<uint32_t, std::vector<std::string>> m_unclesByHeight;
    std::set<std::string> m_orphanBlocks;
    
    // Missing blocks tracking
    std::set<std::string> m_missingBlocks;
    std::map<std::string, double> m_missingBlockRequests;
    static constexpr double MISSING_BLOCK_TIMEOUT = 30.0;
    
    // Reference to gossip app for requesting missing blocks
    TcpGossipApp* m_gossipApp;

    // DAG helper methods
    VertexDescriptor addBlockToDAG(std::shared_ptr<Share> block) {
        VertexDescriptor vertex = boost::add_vertex(m_dag);
        m_dag[vertex] = BlockVertex(block->blockHash, block);
        m_hashToVertex[block->blockHash] = vertex;
        
        // Add parent edge if parent exists
        if (block->parentHash != "genesis" && block->parentHash != "") {
            auto parentIt = m_hashToVertex.find(block->parentHash);
            if (parentIt != m_hashToVertex.end()) {
                boost::add_edge(parentIt->second, vertex, BlockEdge("parent", block->difficulty), m_dag);
            } else {
                // Parent is missing - add to missing blocks
                m_missingBlocks.insert(block->parentHash);
                requestMissingBlock(block->parentHash);
            }
        }
        
        // Add reference edges for additional references
        for (const std::string& refHash : block->references) {
            auto refIt = m_hashToVertex.find(refHash);
            if (refIt != m_hashToVertex.end()) {
                boost::add_edge(refIt->second, vertex, BlockEdge("reference", 0), m_dag);
            } else {
                // Reference is missing
                m_missingBlocks.insert(refHash);
                requestMissingBlock(refHash);
            }
        }
        
        return vertex;
    }
    
    void requestMissingBlock(const std::string& blockHash) {
        if (m_gossipApp && m_missingBlockRequests.find(blockHash) == m_missingBlockRequests.end()) {
            m_missingBlockRequests[blockHash] = Simulator::Now().GetSeconds();
            std::vector<std::string> missingHashes = {blockHash};
            m_gossipApp->RequestMissingBlocks(missingHashes);
            NS_LOG_INFO("Requested missing block: " << blockHash);
        }
    }
    
    VertexDescriptor findHeaviestPath() {
        VertexDescriptor heaviestVertex;
        uint32_t maxWork = 0;
        bool found = false;
        
        // Find all leaf nodes (blocks with no children)
        auto vertexRange = boost::vertices(m_dag);
        for (auto it = vertexRange.first; it != vertexRange.second; ++it) {
            if (boost::out_degree(*it, m_dag) == 0) { // Leaf node
                uint32_t pathWork = calculatePathWork(*it);
                if (!found || pathWork > maxWork) {
                    maxWork = pathWork;
                    heaviestVertex = *it;
                    found = true;
                }
            }
        }
        
        return heaviestVertex;
    }
    

    
    std::vector<VertexDescriptor> getMainChainPath(VertexDescriptor head) const {
        std::vector<VertexDescriptor> path;
        std::set<VertexDescriptor> visited;
        
        VertexDescriptor current = head;
        
        while (current != VertexDescriptor() && visited.find(current) == visited.end()) {
            visited.insert(current);
            path.push_back(current);
            
            // Find parent edge
            auto inEdges = boost::in_edges(current, m_dag);
            VertexDescriptor parent;
            bool foundParent = false;
            
            for (auto edgeIt = inEdges.first; edgeIt != inEdges.second; ++edgeIt) {
                if (m_dag[*edgeIt].edgeType == "parent") {
                    parent = boost::source(*edgeIt, m_dag);
                    foundParent = true;
                    break;
                }
            }
            
            if (foundParent && m_dag[parent].blockHash != "genesis") {
                current = parent;
            } else {
                break;
            }
        }
        
        std::reverse(path.begin(), path.end());
        return path;
    }
    

    
    std::vector<VertexDescriptor> getPathToGenesis(VertexDescriptor vertex) {
        std::vector<VertexDescriptor> path;
        std::set<VertexDescriptor> visited;
        
        VertexDescriptor current = vertex;
        
        while (current != VertexDescriptor() && visited.find(current) == visited.end()) {
            visited.insert(current);
            path.push_back(current);
            
            if (m_dag[current].blockHash == "genesis") {
                break;
            }
            
            // Find parent
            auto inEdges = boost::in_edges(current, m_dag);
            VertexDescriptor parent;
            bool foundParent = false;
            
            for (auto edgeIt = inEdges.first; edgeIt != inEdges.second; ++edgeIt) {
                if (m_dag[*edgeIt].edgeType == "parent") {
                    parent = boost::source(*edgeIt, m_dag);
                    foundParent = true;
                    break;
                }
            }
            
            if (foundParent) {
                current = parent;
            } else {
                break;
            }
        }
        
        std::reverse(path.begin(), path.end());
        return path;
    }
    
    void updateMainChainMarkers(VertexDescriptor newHead) {
        // Clear all main chain markers
        auto vertexRange = boost::vertices(m_dag);
        for (auto it = vertexRange.first; it != vertexRange.second; ++it) {
            m_dag[*it].isOnMainChain = false;
        }
        
        // Mark new main chain
        std::vector<VertexDescriptor> mainChain = getMainChainPath(newHead);
        for (VertexDescriptor vertex : mainChain) {
            m_dag[vertex].isOnMainChain = true;
        }
    }
    
    std::string getShortHash(const std::string& hash, size_t length = 8) const {
        if (hash.length() <= length) return hash;
        return hash.substr(0, length) + "...";
    }
    
    void cleanupTimedOutRequests() {
        double currentTime = Simulator::Now().GetSeconds();
        
        for (auto it = m_missingBlockRequests.begin(); it != m_missingBlockRequests.end();) {
            if (currentTime - it->second > MISSING_BLOCK_TIMEOUT) {
                m_missingBlocks.erase(it->first);
                it = m_missingBlockRequests.erase(it);
            } else {
                ++it;
            }
        }
    }

public:
    BlockchainState(uint32_t maxUnclesPerHeight = 5, TcpGossipApp* gossipApp = nullptr)
        : m_currentHead("genesis"), m_currentHeight(0), 
          m_maxUnclesPerHeight(maxUnclesPerHeight), m_gossipApp(gossipApp) {
        
        // Initialize genesis block in both DAG and traditional structure
        auto genesis = std::make_shared<Share>("genesis", "", 0, 0, 0.0);
        genesis->chainWeight = 0;
        genesis->cumulativeWork = 0;
        
        m_blockchain["genesis"] = genesis;
        m_blocksByHeight[0].push_back("genesis");
        
        // Add genesis to DAG
        VertexDescriptor genesisVertex = boost::add_vertex(m_dag);
        m_dag[genesisVertex] = BlockVertex("genesis", genesis);
        m_dag[genesisVertex].isOnMainChain = true;
        m_hashToVertex["genesis"] = genesisVertex;
    }

        struct ChainAnalysis {
        uint32_t totalBlocks;
        uint32_t mainChainLength;
        uint32_t sideBlocks;
        uint32_t uncleBlocks;
        uint32_t orphanBlocks;
        uint32_t uniqueMiners;
        double averageBlockTime;
        std::map<uint32_t, uint32_t> minerDistribution;
        std::map<uint32_t, uint32_t> heightDistribution;
        std::vector<uint32_t> uncleDistribution; // uncles per height
    };
    
    ChainAnalysis getDetailedAnalysis() const {
        ChainAnalysis analysis;
        
        analysis.totalBlocks = m_blockchain.size() - 1; // Exclude genesis
        analysis.mainChainLength = m_currentHeight + 1;
        analysis.sideBlocks = analysis.totalBlocks - analysis.mainChainLength;
        analysis.orphanBlocks = m_orphanBlocks.size();
        
        // Count uncles
        uint32_t totalUncles = 0;
        for (const auto& pair : m_unclesByHeight) {
            totalUncles += pair.second.size();
        }
        analysis.uncleBlocks = totalUncles;
        
        // Miner distribution and timing analysis
        std::map<uint32_t, std::vector<double>> minerTimes;
        double totalTime = 0;
        uint32_t timeBlocks = 0;
        
        for (const auto& pair : m_blockchain) {
            if (pair.first != "genesis") {
                const auto& block = pair.second;
                analysis.minerDistribution[block->nodeId]++;
                analysis.heightDistribution[block->blockNumber]++;
                
                if (block->timestamp > 0) {
                    minerTimes[block->nodeId].push_back(block->timestamp);
                    totalTime += block->timestamp;
                    timeBlocks++;
                }
            }
        }
        
        analysis.uniqueMiners = analysis.minerDistribution.size();
        analysis.averageBlockTime = (timeBlocks > 0) ? (totalTime / timeBlocks) : 0.0;
        
        // Uncle distribution per height
        for (uint32_t h = 0; h <= m_currentHeight; ++h) {
            analysis.uncleDistribution.push_back(getUncleCountAtHeight(h));
        }
        
        return analysis;
    }
    
    // Get fork analysis
    struct ForkInfo {
        std::vector<std::string> forkPoints;
        std::map<std::string, std::vector<std::string>> forkBranches;
        uint32_t maxForkDepth;
        double forkRatio; // side blocks / total blocks
    };
    
    ForkInfo getForkAnalysis() const {
        ForkInfo forkInfo;
        forkInfo.maxForkDepth = 0;
        
        uint32_t totalBlocks = m_blockchain.size() - 1; // Exclude genesis
        uint32_t mainChainBlocks = m_currentHeight; // Blocks on main chain (excluding genesis)
        uint32_t sideBlocks = 0;
        
        // Count blocks not on main chain
        for (const auto& pair : m_blockchain) {
            if (pair.first != "genesis" && !isOnMainChain(pair.first)) {
                sideBlocks++;
            }
        }
        
        // Find actual fork points by looking at heights with multiple blocks
        for (const auto& heightPair : m_blocksByHeight) {
            uint32_t height = heightPair.first;
            const auto& blocksAtHeight = heightPair.second;
            
            if (height > 0 && blocksAtHeight.size() > 1) {
                // This height has multiple blocks = fork
                std::string parentHash = "";
                bool foundParent = false;
                
                // Find common parent of blocks at this height
                for (const auto& blockHash : blocksAtHeight) {
                    auto blockIt = m_blockchain.find(blockHash);
                    if (blockIt != m_blockchain.end()) {
                        if (!foundParent) {
                            parentHash = blockIt->second->parentHash;
                            foundParent = true;
                        }
                        forkInfo.forkBranches[parentHash].push_back(blockHash);
                    }
                }
                
                if (foundParent && forkInfo.forkBranches[parentHash].size() > 1) {
                    forkInfo.forkPoints.push_back(parentHash);
                    
                    uint32_t forkDepth = blocksAtHeight.size() - 1;
                    if (forkDepth > forkInfo.maxForkDepth) {
                        forkInfo.maxForkDepth = forkDepth;
                    }
                }
            }
        }
        
        // Correct fork ratio calculation
        forkInfo.forkRatio = (totalBlocks > 0) ? (double)sideBlocks / totalBlocks : 0.0;
        
        return forkInfo;
    }
    
    // Get network health metrics
    struct NetworkHealth {
        double consensusRatio;    // nodes on longest chain / total nodes
        double uncleEfficiency;   // uncles / max possible uncles
        double orphanRate;        // orphans / total blocks
        uint32_t reorgCount;      // estimate based on chain weight changes
        bool isHealthy;          // overall assessment
    };
    
    NetworkHealth getNetworkHealth() const {
        NetworkHealth health;
        
        uint32_t totalBlocks = m_blockchain.size() - 1;
        health.orphanRate = (totalBlocks > 0) ? (double)m_orphanBlocks.size() / totalBlocks : 0.0;
        
        // Calculate uncle efficiency
        uint32_t maxPossibleUncles = 0;
        uint32_t actualUncles = 0;
        
        for (uint32_t h = 1; h <= m_currentHeight; ++h) {
            maxPossibleUncles += m_maxUnclesPerHeight;
            actualUncles += getUncleCountAtHeight(h);
        }
        
        health.uncleEfficiency = (maxPossibleUncles > 0) ? (double)actualUncles / maxPossibleUncles : 0.0;
        
        // Simple health assessment
        health.isHealthy = (health.orphanRate < 0.1) && (health.uncleEfficiency < 0.8);
        
        return health;
    }
    
    void setGossipApp(TcpGossipApp* gossipApp) {
        m_gossipApp = gossipApp;
    }
    
    std::vector<std::string> getPendingMissingBlocks() const {
        std::vector<std::string> pending;
        double currentTime = Simulator::Now().GetSeconds();

        for (const auto& pair : m_missingBlockRequests) {
            if (currentTime - pair.second < MISSING_BLOCK_TIMEOUT) {
                pending.push_back(pair.first);
            }
        }
        return pending;
    }

    void exportDAGToDot(const std::string& filename, uint32_t nodeId = 0) const {
        std::ofstream dotFile(filename);
        if (!dotFile.is_open()) {
            NS_LOG_ERROR("Failed to open DOT file: " << filename);
            return;
        }

        dotFile << "digraph BlockchainDAG {\n";
        dotFile << "    rankdir=TB;\n";  // Top to Bottom layout
        dotFile << "    node [shape=box, style=filled];\n";
        dotFile << "    edge [fontsize=10];\n";
        dotFile << "\n";
        
        // Add title
        dotFile << "    labelloc=\"t\";\n";
        dotFile << "    label=\"Blockchain DAG - Node " << nodeId << "\\n";
        dotFile << "Height: " << m_currentHeight << ", Head: " << getShortHash(m_currentHead) << "\";\n";
        dotFile << "\n";

        // Define color scheme
        std::map<std::string, std::string> colors = {
            {"genesis", "lightblue"},
            {"mainchain", "lightgreen"},
            {"uncle", "yellow"},
            {"orphan", "lightcoral"},
            {"missing", "lightgray"}
        };

        // Export vertices (blocks)
        auto vertexRange = boost::vertices(m_dag);
        for (auto it = vertexRange.first; it != vertexRange.second; ++it) {
            const BlockVertex& vertex = m_dag[*it];
            std::string blockHash = vertex.blockHash;
            std::string shortHash = getShortHash(blockHash, 6);
            
            // Determine block type and color
            std::string color;
            std::string label;
            
            if (blockHash == "genesis") {
                color = colors["genesis"];
                label = "Genesis\\n(0)";
            } else if (vertex.isOnMainChain) {
                color = colors["mainchain"];
                label = shortHash + "\\nH:" + std::to_string(vertex.share->blockNumber) + 
                    "\\nM:" + std::to_string(vertex.share->nodeId) +
                    "\\nW:" + std::to_string(vertex.cumulativeWork);
            } else if (m_orphanBlocks.find(blockHash) != m_orphanBlocks.end()) {
                color = colors["orphan"];
                label = shortHash + "\\nH:" + std::to_string(vertex.share->blockNumber) + 
                    "\\nM:" + std::to_string(vertex.share->nodeId) +
                    "\\n(ORPHAN)";
            } else {
                color = colors["uncle"];
                label = shortHash + "\\nH:" + std::to_string(vertex.share->blockNumber) + 
                    "\\nM:" + std::to_string(vertex.share->nodeId) +
                    "\\n(UNCLE)";
            }

            dotFile << "    \"" << blockHash << "\" [";
            dotFile << "label=\"" << label << "\", ";
            dotFile << "fillcolor=\"" << color << "\", ";
            dotFile << "tooltip=\"Hash: " << blockHash << "\\n";
            dotFile << "Height: " << vertex.share->blockNumber << "\\n";
            dotFile << "Miner: " << vertex.share->nodeId << "\\n";
            dotFile << "Work: " << vertex.cumulativeWork << "\\n";
            dotFile << "Timestamp: " << std::fixed << std::setprecision(2) << vertex.share->timestamp << "\"];\n";
        }

        dotFile << "\n";

        // Export edges (relationships)
        auto edgeRange = boost::edges(m_dag);
        for (auto it = edgeRange.first; it != edgeRange.second; ++it) {
            VertexDescriptor source = boost::source(*it, m_dag);
            VertexDescriptor target = boost::target(*it, m_dag);
            const BlockEdge& edge = m_dag[*it];
            
            std::string sourceHash = m_dag[source].blockHash;
            std::string targetHash = m_dag[target].blockHash;
            
            dotFile << "    \"" << sourceHash << "\" -> \"" << targetHash << "\"";
            
            // Style edges based on type
            if (edge.edgeType == "parent") {
                dotFile << " [color=black, penwidth=2, label=\"parent\"]";
            } else if (edge.edgeType == "reference") {
                dotFile << " [color=blue, style=dashed, label=\"ref\"]";
            }
            
            dotFile << ";\n";
        }

        // Add missing blocks if any
        if (!m_missingBlocks.empty()) {
            dotFile << "\n    // Missing blocks\n";
            for (const std::string& missingHash : m_missingBlocks) {
                std::string shortHash = getShortHash(missingHash, 6);
                dotFile << "    \"" << missingHash << "\" [";
                dotFile << "label=\"" << shortHash << "\\n(MISSING)\", ";
                dotFile << "fillcolor=\"" << colors["missing"] << "\", ";
                dotFile << "style=\"filled,dashed\"];\n";
            }
        }

        // Add legend
        dotFile << "\n    // Legend\n";
        dotFile << "    subgraph cluster_legend {\n";
        dotFile << "        label=\"Legend\";\n";
        dotFile << "        style=filled;\n";
        dotFile << "        fillcolor=white;\n";
        dotFile << "        \n";
        dotFile << "        legend_genesis [label=\"Genesis\", fillcolor=\"" << colors["genesis"] << "\", style=filled, shape=box];\n";
        dotFile << "        legend_main [label=\"Main Chain\", fillcolor=\"" << colors["mainchain"] << "\", style=filled, shape=box];\n";
        dotFile << "        legend_uncle [label=\"Uncle\", fillcolor=\"" << colors["uncle"] << "\", style=filled, shape=box];\n";
        dotFile << "        legend_orphan [label=\"Orphan\", fillcolor=\"" << colors["orphan"] << "\", style=filled, shape=box];\n";
        if (!m_missingBlocks.empty()) {
            dotFile << "        legend_missing [label=\"Missing\", fillcolor=\"" << colors["missing"] << "\", style=\"filled,dashed\", shape=box];\n";
        }
        dotFile << "        \n";
        dotFile << "        legend_genesis -> legend_main -> legend_uncle -> legend_orphan [style=invis];\n";
        dotFile << "    }\n";

        // Add statistics
        dotFile << "\n    // Statistics\n";
        dotFile << "    subgraph cluster_stats {\n";
        dotFile << "        label=\"Statistics\";\n";
        dotFile << "        style=filled;\n";
        dotFile << "        fillcolor=lightyellow;\n";
        dotFile << "        \n";
        
        uint32_t totalBlocks = boost::num_vertices(m_dag) - 1; // exclude genesis
        uint32_t mainChainBlocks = 0;
        uint32_t uncleBlocks = 0;
        uint32_t orphanBlocks = m_orphanBlocks.size();
        
        // Count main chain blocks
        for (auto it = vertexRange.first; it != vertexRange.second; ++it) {
            if (m_dag[*it].isOnMainChain && m_dag[*it].blockHash != "genesis") {
                mainChainBlocks++;
            } else if (m_dag[*it].blockHash != "genesis" && 
                    m_orphanBlocks.find(m_dag[*it].blockHash) == m_orphanBlocks.end()) {
                uncleBlocks++;
            }
        }
        
        dotFile << "        stats [shape=plaintext, label=<\n";
        dotFile << "            <table border=\"0\" cellborder=\"1\" cellspacing=\"0\">\n";
        dotFile << "                <tr><td><b>Metric</b></td><td><b>Value</b></td></tr>\n";
        dotFile << "                <tr><td>Total Blocks</td><td>" << totalBlocks << "</td></tr>\n";
        dotFile << "                <tr><td>Main Chain</td><td>" << mainChainBlocks << "</td></tr>\n";
        dotFile << "                <tr><td>Uncle Blocks</td><td>" << uncleBlocks << "</td></tr>\n";
        dotFile << "                <tr><td>Orphan Blocks</td><td>" << orphanBlocks << "</td></tr>\n";
        dotFile << "                <tr><td>Missing Blocks</td><td>" << m_missingBlocks.size() << "</td></tr>\n";
        dotFile << "                <tr><td>Current Height</td><td>" << m_currentHeight << "</td></tr>\n";
        dotFile << "            </table>\n";
        dotFile << "        >];\n";
        dotFile << "    }\n";

        dotFile << "}\n";
        dotFile.close();
        
        NS_LOG_INFO("DAG exported to DOT file: " << filename);
    }

    // Helper method to export multiple snapshots during simulation
    void exportDAGSnapshot(const std::string& baseFilename, uint32_t nodeId, double timestamp) const {
        std::stringstream ss;
        ss << baseFilename << "_node" << nodeId << "_t" << std::fixed << std::setprecision(1) << timestamp << ".dot";
        exportDAGToDot(ss.str(), nodeId);
    }

   
    BlockRelation analyzeBlock(const std::shared_ptr<Share>& newBlock) {
        cleanupTimedOutRequests();
        
        // Check if we already have this block
        if (m_hashToVertex.find(newBlock->blockHash) != m_hashToVertex.end()) {
            return BlockRelation::DUPLICATE;
        }
        
        // Check if parent exists
        if (newBlock->parentHash != "genesis" && newBlock->parentHash != "") {
            if (m_hashToVertex.find(newBlock->parentHash) == m_hashToVertex.end()) {
                return BlockRelation::ORPHAN;  // Parent missing = orphan
            }
        }
        
        // Calculate cumulative work for this block
        auto blockCopy = std::make_shared<Share>(*newBlock, newBlock->receivedTime);
        blockCopy->calculateChainWeight(m_blockchain);
        
        // Case 1: Block extends current head (most common case)
        if (newBlock->parentHash == m_currentHead) {
            if (newBlock->blockNumber == m_currentHeight + 1) {
                return BlockRelation::PARENT;  // Direct extension
            }
        }
        
        // Case 2: Block at same height as current head (competing block)
        if (newBlock->blockNumber == m_currentHeight) {
            // Compare work to determine if reorganization is needed
            VertexDescriptor currentHeadVertex = m_hashToVertex[m_currentHead];
            uint32_t currentHeadWork = calculatePathWork(currentHeadVertex);
            uint32_t newBlockWork = blockCopy->cumulativeWork;
            
            if (newBlockWork > currentHeadWork) {
                return BlockRelation::REORG_NEEDED;
            } else {
                return BlockRelation::SIBLING;  // Same height, less work = sibling
            }
        }
        
        // Case 3: Block extends a side chain but creates heavier chain
        uint32_t newBlockWork = blockCopy->cumulativeWork;
        VertexDescriptor currentHeadVertex = m_hashToVertex[m_currentHead];
        uint32_t currentHeadWork = calculatePathWork(currentHeadVertex);
        
        if (newBlockWork > currentHeadWork) {
            return BlockRelation::REORG_NEEDED;
        }
        
        // Case 4: Block builds on main chain but at wrong height
        if (isOnMainChain(newBlock->parentHash)) {
            if (newBlock->blockNumber <= m_currentHeight) {
                // Building on old main chain block
                return BlockRelation::UNCLE;
            }
        }
        
        // Case 5: Block builds on side chain
        if (m_hashToVertex.find(newBlock->parentHash) != m_hashToVertex.end()) {
            // Parent exists but not on main chain
            uint32_t heightDiff = abs((int)newBlock->blockNumber - (int)m_currentHeight);
            
            if (heightDiff <= 2) {  // Within uncle range
                return BlockRelation::UNCLE;
            } else {
                return BlockRelation::ORPHAN;  // Too far from main chain
            }
        }
        
        return BlockRelation::INVALID;
    }

    
    bool addBlock(const std::shared_ptr<Share>& newBlock) {
        BlockRelation relation = analyzeBlock(newBlock);
        
        switch (relation) {
            case BlockRelation::DUPLICATE:
                NS_LOG_DEBUG("Duplicate block: " << newBlock->blockHash);
                return false;
                
            case BlockRelation::ORPHAN:
                // Store orphan blocks in DAG and traditional structure
                newBlock->calculateChainWeight(m_blockchain);
                m_blockchain[newBlock->blockHash] = newBlock;
                m_blocksByHeight[newBlock->blockNumber].push_back(newBlock->blockHash);
                addBlockToDAG(newBlock);
                NS_LOG_INFO("Added orphan block to DAG: " << newBlock->blockHash);
                return false;
                
            case BlockRelation::PARENT: {
                // New head of the chain
                newBlock->calculateChainWeight(m_blockchain);
                m_blockchain[newBlock->blockHash] = newBlock;
                m_blocksByHeight[newBlock->blockNumber].push_back(newBlock->blockHash);

                VertexDescriptor newVertex = addBlockToDAG(newBlock);
                m_currentHead = newBlock->blockHash;
                m_currentHeight = newBlock->blockNumber;

                updateMainChainMarkers(newVertex);

                NS_LOG_INFO("New chain head in DAG: " << newBlock->blockHash << " at height " << m_currentHeight);
                return true;
            }

            case BlockRelation::REORG_NEEDED:
                return performDAGReorganization(newBlock);
                
            case BlockRelation::SIBLING:
            case BlockRelation::UNCLE:
                // Add to DAG as uncle
                newBlock->calculateChainWeight(m_blockchain);
                m_blockchain[newBlock->blockHash] = newBlock;
                m_blocksByHeight[newBlock->blockNumber].push_back(newBlock->blockHash);
                addBlockToDAG(newBlock);
                addUncleAtHeight(newBlock->blockNumber, newBlock->blockHash);
                
                NS_LOG_INFO("Added uncle block to DAG " << newBlock->blockHash 
                           << " at height " << newBlock->blockNumber);
                return false;
                
            case BlockRelation::REJECTED_UNCLE:
                // Mark as orphan
                newBlock->calculateChainWeight(m_blockchain);
                m_blockchain[newBlock->blockHash] = newBlock;
                m_blocksByHeight[newBlock->blockNumber].push_back(newBlock->blockHash);
                addBlockToDAG(newBlock);
                markAsOrphan(newBlock->blockHash);
                
                NS_LOG_INFO("Orphaned block in DAG due to uncle limit: " << newBlock->blockHash);
                return false;
                
            default:
                return false;
        }
    }
    
    bool performDAGReorganization(const std::shared_ptr<Share>& newBlock) {
        NS_LOG_INFO("Starting DAG reorganization for block: " << newBlock->blockHash);
        
        // Add the new block to DAG
        newBlock->calculateChainWeight(m_blockchain);
        m_blockchain[newBlock->blockHash] = newBlock;
        m_blocksByHeight[newBlock->blockNumber].push_back(newBlock->blockHash);
        VertexDescriptor newVertex = addBlockToDAG(newBlock);
        
        // Find the new heaviest path
        VertexDescriptor heaviestVertex = findHeaviestPath();
        
        if (m_dag[heaviestVertex].blockHash != m_currentHead) {
            // Find common ancestor between current head and new head
            VertexDescriptor oldHeadVertex = m_hashToVertex[m_currentHead];
            VertexDescriptor commonAncestor = findCommonAncestor(oldHeadVertex, heaviestVertex);
            
            NS_LOG_INFO("DAG Reorg: Common ancestor is " << m_dag[commonAncestor].blockHash);
            
            // Check if we need to request missing blocks
            std::vector<std::string> missingBlocks = getMissingBlocksInPath(commonAncestor, heaviestVertex);
            
            if (!missingBlocks.empty() && m_gossipApp) {
                NS_LOG_INFO("Requesting " << missingBlocks.size() << " missing blocks for reorg");
                m_gossipApp->RequestMissingBlocks(missingBlocks);
                return false; // Wait for missing blocks
            }
            
            // Perform the reorganization
            std::string oldHead = m_currentHead;
            m_currentHead = m_dag[heaviestVertex].blockHash;
            m_currentHeight = m_dag[heaviestVertex].share->blockNumber;
            
            updateMainChainMarkers(heaviestVertex);
            updateUncleTrackingForReorg(oldHead, m_currentHead);
            
            NS_LOG_INFO("DAG REORGANIZATION: Changed head from " << oldHead 
                       << " to " << m_currentHead);
            return true;
        }
        
        return false;
    }
    
    void printDAGState(uint32_t nodeId) const {
        NS_LOG_INFO("=== DAG STATE FOR NODE " << nodeId << " ===");
        NS_LOG_INFO("Total vertices: " << boost::num_vertices(m_dag));
        NS_LOG_INFO("Total edges: " << boost::num_edges(m_dag));
        NS_LOG_INFO("Current head: " << m_currentHead);
        NS_LOG_INFO("Missing blocks: " << m_missingBlocks.size());
        
        // Print main chain
        if (m_hashToVertex.find(m_currentHead) != m_hashToVertex.end()) {
            VertexDescriptor headVertex = m_hashToVertex.at(m_currentHead);
            std::vector<VertexDescriptor> mainChain = getMainChainPath(headVertex);
            
            NS_LOG_INFO("--- MAIN CHAIN IN DAG ---");
            for (size_t i = 0; i < mainChain.size(); ++i) {
                const BlockVertex& vertex = m_dag[mainChain[i]];
                NS_LOG_INFO("  " << i << ": " << vertex.blockHash 
                           << " (Work: " << vertex.cumulativeWork 
                           << ", Height: " << vertex.share->blockNumber << ")");
            }
        }
        
        // Print orphan blocks
        auto orphanCount = std::count_if(boost::vertices(m_dag).first, boost::vertices(m_dag).second,
            [this](VertexDescriptor v) { return !m_dag[v].isOnMainChain && m_dag[v].blockHash != "genesis"; });
        
        NS_LOG_INFO("Orphan blocks in DAG: " << orphanCount);
        NS_LOG_INFO("=== END DAG STATE ===\n");
    }

        VertexDescriptor findCommonAncestor(VertexDescriptor vertex1, VertexDescriptor vertex2) {
        // Get paths from both vertices to genesis
        std::vector<VertexDescriptor> path1 = getPathToGenesis(vertex1);
        std::vector<VertexDescriptor> path2 = getPathToGenesis(vertex2);
        
        // Find last common vertex in paths
        size_t minSize = std::min(path1.size(), path2.size());
        VertexDescriptor commonAncestor;
        
        for (size_t i = 0; i < minSize; ++i) {
            if (path1[i] == path2[i]) {
                commonAncestor = path1[i];
            } else {
                break;
            }
        }
        
        return commonAncestor;
    }


    std::vector<std::string> getMissingBlocksInPath(VertexDescriptor from, VertexDescriptor to) {
        std::vector<std::string> missingBlocks;
        std::set<VertexDescriptor> visited;
        
        // BFS to find all blocks between from and to
        std::queue<VertexDescriptor> queue;
        queue.push(to);
        visited.insert(to);
        
        while (!queue.empty()) {
            VertexDescriptor current = queue.front();
            queue.pop();
            
            if (current == from) {
                break;
            }
            
            auto inEdges = boost::in_edges(current, m_dag);
            for (auto edgeIt = inEdges.first; edgeIt != inEdges.second; ++edgeIt) {
                VertexDescriptor parent = boost::source(*edgeIt, m_dag);
                
                if (visited.find(parent) == visited.end()) {
                    visited.insert(parent);
                    queue.push(parent);
                }
            }
        }
        
        return missingBlocks;
    }

    uint32_t calculatePathWork(VertexDescriptor vertex) {
        // Calculate cumulative work from genesis to this vertex
        uint32_t totalWork = 0;
        std::set<VertexDescriptor> visited;
        
        std::function<uint32_t(VertexDescriptor)> dfs = [&](VertexDescriptor v) -> uint32_t {
            if (visited.find(v) != visited.end()) {
                return 0; // Avoid cycles
            }
            visited.insert(v);
            
            uint32_t maxParentWork = 0;
            auto inEdges = boost::in_edges(v, m_dag);
            
            for (auto edgeIt = inEdges.first; edgeIt != inEdges.second; ++edgeIt) {
                if (m_dag[*edgeIt].edgeType == "parent") {
                    VertexDescriptor parent = boost::source(*edgeIt, m_dag);
                    uint32_t parentWork = dfs(parent);
                    maxParentWork = std::max(maxParentWork, parentWork);
                }
            }
            
            return maxParentWork + m_dag[v].share->difficulty;
        };
        
        return dfs(vertex);
    }
    
    // Helper methods (keeping compatibility with existing interface)
    uint32_t getUncleCountAtHeight(uint32_t height) const {
        auto it = m_unclesByHeight.find(height);
        return (it != m_unclesByHeight.end()) ? it->second.size() : 0;
    }
    
    bool canAcceptUncleAtHeight(uint32_t height) const {
        return getUncleCountAtHeight(height) < m_maxUnclesPerHeight;
    }
    
    void addUncleAtHeight(uint32_t height, const std::string& blockHash) {
        m_unclesByHeight[height].push_back(blockHash);
    }
    
    void markAsOrphan(const std::string& blockHash) {
        m_orphanBlocks.insert(blockHash);
    }
    
    void updateUncleTrackingForReorg(const std::string& oldHead, const std::string& newHead) {
        m_unclesByHeight.clear();
        m_orphanBlocks.clear();
        
        // Rebuild uncle tracking based on new main chain
        for (const auto& heightPair : m_blocksByHeight) {
            uint32_t height = heightPair.first;
            const auto& blocksAtHeight = heightPair.second;
            
            uint32_t uncleCount = 0;
            for (const auto& blockHash : blocksAtHeight) {
                if (!isOnMainChain(blockHash) && blockHash != "genesis") {
                    if (uncleCount < m_maxUnclesPerHeight) {
                        addUncleAtHeight(height, blockHash);
                        uncleCount++;
                    } else {
                        markAsOrphan(blockHash);
                    }
                }
            }
        }
    }
    
    bool isOnMainChain(const std::string& blockHash) const {
        auto it = m_hashToVertex.find(blockHash);
        if (it != m_hashToVertex.end()) {
            return m_dag[it->second].isOnMainChain;
        }
        return false;
    }
    
    // Getters
    std::string getCurrentHead() const { return m_currentHead; }
    uint32_t getCurrentHeight() const { return m_currentHeight; }
    
    const BlockDAG& getDAG() const { return m_dag; }
    
    std::vector<std::string> getMissingBlocks() const {
        return std::vector<std::string>(m_missingBlocks.begin(), m_missingBlocks.end());
    }
    
    void printCompleteState(uint32_t nodeId) const {
        printDAGState(nodeId);
    }

    // Get blocks by height (needed for addDAGReferences)
    const std::vector<std::string>& getBlocksByHeight(uint32_t height) const {
        static std::vector<std::string> emptyVector;
        auto it = m_blocksByHeight.find(height);
        return (it != m_blocksByHeight.end()) ? it->second : emptyVector;
    }
    
    // Get hash to vertex mapping (needed for orphan resolution)
    const std::map<std::string, VertexDescriptor>& getHashToVertexMap() const {
        return m_hashToVertex;
    }
    
    // Get current head vertex
    VertexDescriptor getCurrentHeadVertex() const {
        auto it = m_hashToVertex.find(m_currentHead);
        return (it != m_hashToVertex.end()) ? it->second : VertexDescriptor();
    }
    
    // Check if block exists in blockchain
    bool hasBlock(const std::string& blockHash) const {
        return m_blockchain.find(blockHash) != m_blockchain.end();
    }
    
    // Get block data for network transmission
    std::string getBlockData(const std::string& blockHash) const {
        auto it = m_blockchain.find(blockHash);
        return (it != m_blockchain.end()) ? it->second->serialize() : "";
    }
    
    // Update block classification after resolution
    bool updateBlockClassification(const std::string& blockHash, BlockRelation newRelation) {
        auto it = m_hashToVertex.find(blockHash);
        if (it != m_hashToVertex.end()) {
            // Update DAG vertex properties based on new classification
            VertexDescriptor vertex = it->second;
            m_dag[vertex].isProcessed = true;
            
            // Update tracking structures based on new relation
            switch (newRelation) {
                case BlockRelation::UNCLE:
                    addUncleAtHeight(m_dag[vertex].share->blockNumber, blockHash);
                    break;
                case BlockRelation::PARENT:
                    // This block might become new head
                    return performDAGReorganization(m_dag[vertex].share);
                default:
                    break;
            }
            return true;
        }
        return false;
    }
    
    // Perform reprocessing of resolved blocks
    bool performReprocessing(std::shared_ptr<Share> block) {
        // Re-analyze and potentially trigger reorganization
        return performDAGReorganization(block);
    }
    
    // Get path from ancestor to descendant
    std::vector<VertexDescriptor> getPathFromAncestor(VertexDescriptor ancestor, VertexDescriptor descendant) {
        std::vector<VertexDescriptor> path;
        
        std::vector<VertexDescriptor> fullPath = getPathToGenesis(descendant);
        
        // Find ancestor in the path and return sub-path from ancestor to descendant
        bool foundAncestor = false;
        for (VertexDescriptor vertex : fullPath) {
            if (vertex == ancestor) {
                foundAncestor = true;
            }
            if (foundAncestor) {
                path.push_back(vertex);
            }
        }
        
        return path;
    }
    
};

class MinerApp : public Application {
private:
    EventId m_miningEvent;
    uint32_t m_blockCounter = 0;
    bool m_running = false;
    Ptr<TcpGossipApp> m_gossipApp;
    double m_stopMiningTime = 0.0;
    
    // DAG-based blockchain state management
    std::unique_ptr<BlockchainState> m_blockchain;
    
    // Per-node normal distribution random number generator for mining intervals
    Ptr<NormalRandomVariable> m_miningIntervalRNG;
    
    // Network delay simulation for realistic fork creation
    Ptr<UniformRandomVariable> m_networkDelayRNG;
    Ptr<UniformRandomVariable> m_miningVariationRNG;
    
    // DAG-specific mining parameters
    uint32_t m_difficulty;
    double m_lastReorgTime;
    uint32_t m_reorgCount;
    
    // Track mining restarts due to reorganizations
    uint32_t m_miningRestarts;
    
    // Track orphan statistics (naturally occurring only)
    uint32_t m_orphansReceived;
    
    // Store the time when we started mining current block
    double m_currentMiningStartTime;
    
protected:
    // Normal distribution parameters for mining intervals
    static double s_meanMiningInterval;     // Mean mining time (seconds)
    static double s_stdMiningInterval;      // Standard deviation (seconds)
    static double s_minMiningInterval;      // Minimum allowed interval
    static double s_maxMiningInterval;      // Maximum allowed interval
    
    // Network simulation parameters for realistic forks
    static double s_maxNetworkDelay;        // Maximum network propagation delay
    static double s_miningVariation;        // Additional mining time variation
    
public:
    MinerApp() : m_difficulty(1), m_lastReorgTime(0.0), m_reorgCount(0), m_miningRestarts(0),
                 m_orphansReceived(0), m_currentMiningStartTime(0.0) {
        // Initialize DAG-based blockchain state
        m_blockchain = std::make_unique<BlockchainState>(5); // Max 5 uncles per height
        
        // Initialize per-node normal distribution random number generator
        m_miningIntervalRNG = CreateObject<NormalRandomVariable>();
        m_miningIntervalRNG->SetAttribute("Mean", DoubleValue(s_meanMiningInterval));
        m_miningIntervalRNG->SetAttribute("Variance", DoubleValue(s_stdMiningInterval * s_stdMiningInterval));
        
        // Initialize network delay simulation
        m_networkDelayRNG = CreateObject<UniformRandomVariable>();
        m_networkDelayRNG->SetAttribute("Min", DoubleValue(0.0));
        m_networkDelayRNG->SetAttribute("Max", DoubleValue(s_maxNetworkDelay));
        
        // Initialize mining variation RNG
        m_miningVariationRNG = CreateObject<UniformRandomVariable>();
        m_miningVariationRNG->SetAttribute("Min", DoubleValue(-s_miningVariation));
        m_miningVariationRNG->SetAttribute("Max", DoubleValue(s_miningVariation));
    }
    
    static uint32_t totalBlocksMined;
    static std::map<uint32_t, uint32_t> perNodeMinedBlocks;
    static std::map<uint32_t, uint32_t> perNodeReorgs;
    static std::map<uint32_t, uint32_t> perNodeOrphansReceived;
    
    virtual void StartApplication() override {
        m_running = true;
        uint32_t nodeId = GetNode()->GetId();
        NS_LOG_INFO("MinerApp started on node " << nodeId);
        
        // Set up gossip app reference in blockchain state
        if (m_gossipApp) {
            m_blockchain->setGossipApp(PeekPointer(m_gossipApp));
        }
        
        // Start mining immediately with a random delay from normal distribution
        double initialDelay = GetNextMiningInterval();
        m_miningEvent = Simulator::Schedule(Seconds(initialDelay), &MinerApp::MineBlock, this);
        
        NS_LOG_INFO("Node " << nodeId << " will start DAG mining in " << initialDelay << " seconds");
    }
    
    virtual void StopApplication() override {
        m_running = false;
        if (m_miningEvent.IsRunning()) {
            Simulator::Cancel(m_miningEvent);
        }
        
        uint32_t nodeId = GetNode()->GetId();
        
        // Print final DAG state
        NS_LOG_INFO("=== FINAL DAG STATISTICS FOR NODE " << nodeId << " ===");
        NS_LOG_INFO("Blocks mined: " << m_blockCounter);
        NS_LOG_INFO("Reorganizations performed: " << m_reorgCount);
        NS_LOG_INFO("Mining restarts: " << m_miningRestarts);
        NS_LOG_INFO("Orphans received: " << m_orphansReceived);
        NS_LOG_INFO("Current chain height: " << m_blockchain->getCurrentHeight());
        NS_LOG_INFO("Current head: " << m_blockchain->getCurrentHead());
        
        m_blockchain->printCompleteState(nodeId);
    }

    // Method to get blockchain state for analysis
    const BlockchainState* GetBlockchainState() const {
        return m_blockchain.get();
    }
    
    // Get detailed node statistics including DAG metrics
    std::map<std::string, double> getNodeStats() const {
        std::map<std::string, double> stats;
        stats["blocks_mined"] = static_cast<double>(m_blockCounter);
        stats["reorganizations"] = static_cast<double>(m_reorgCount);
        stats["mining_restarts"] = static_cast<double>(m_miningRestarts);
        stats["orphans_received"] = static_cast<double>(m_orphansReceived);
        stats["current_height"] = static_cast<double>(m_blockchain->getCurrentHeight());
        stats["missing_blocks"] = static_cast<double>(m_blockchain->getMissingBlocks().size());
        
        // Add DAG-specific metrics
        const BlockDAG& dag = m_blockchain->getDAG();
        stats["dag_vertices"] = static_cast<double>(boost::num_vertices(dag));
        stats["dag_edges"] = static_cast<double>(boost::num_edges(dag));
        
        return stats;
    }
    
    void SetSimulationStopTime(double stopTime) {
        m_stopMiningTime = stopTime - 5.0;
    }
    
    void SetGossipApp(Ptr<TcpGossipApp> app) {
        m_gossipApp = app;
        // Set up bidirectional reference
        if (m_gossipApp) {
            m_gossipApp->SetMinerApp(this);
            m_blockchain->setGossipApp(PeekPointer(m_gossipApp));
        }
    }
    
    uint32_t GetBlocksMined() const {
        return m_blockCounter;
    }
    
    uint32_t GetReorgCount() const {
        return m_reorgCount;
    }
    
    uint32_t GetOrphansReceived() const {
        return m_orphansReceived;
    }
    
    // Static methods to configure mining distribution parameters
    static void SetMiningParameters(double mean, double stdDev, double minInterval = 5.0, double maxInterval = 35.0) {
        s_meanMiningInterval = mean;
        s_stdMiningInterval = stdDev;
        s_minMiningInterval = minInterval;
        s_maxMiningInterval = maxInterval;
    }
    
    // Configure network parameters for realistic fork creation
    static void SetNetworkParameters(double maxDelay = 2.0, double miningVariation = 1.0) {
        s_maxNetworkDelay = maxDelay;
        s_miningVariation = miningVariation;
    }
    
    void OnBlockReceived(const std::string& blockData) {
        double currentTime = Simulator::Now().GetSeconds();
        uint32_t nodeId = GetNode()->GetId();
        
        auto receivedBlock = Share::deserialize(blockData, currentTime);
        
        NS_LOG_INFO("Node " << nodeId << " received block " 
                   << receivedBlock->blockHash << " from node " << receivedBlock->nodeId
                   << " (height: " << receivedBlock->blockNumber << ")");
        
        BlockRelation relation = m_blockchain->analyzeBlock(receivedBlock);
        std::string relationStr = getRelationString(relation);
        NS_LOG_INFO("Node " << nodeId << " determined block relationship: " << relationStr);
        
        // Track naturally occurring orphan reception
        if (relation == BlockRelation::ORPHAN) {
            m_orphansReceived++;
            perNodeOrphansReceived[nodeId]++;
            NS_LOG_INFO("Node " << nodeId << " received ORPHAN block - total orphans received: " << m_orphansReceived);
        }
        
        bool chainUpdated = false;
        bool shouldRestartMining = false;
        
        switch (relation) {
            case BlockRelation::PARENT:
                NS_LOG_INFO("Node " << nodeId << " accepting new head block extending current chain");
                chainUpdated = m_blockchain->addBlock(receivedBlock);
                shouldRestartMining = true;
                break;
                
            case BlockRelation::REORG_NEEDED:
                {
                    NS_LOG_INFO("Node " << nodeId << " performing DAG reorganization");
                    std::string oldHead = m_blockchain->getCurrentHead();
                    chainUpdated = m_blockchain->addBlock(receivedBlock);
                    
                    if (chainUpdated) {
                        m_reorgCount++;
                        perNodeReorgs[nodeId]++;
                        m_lastReorgTime = currentTime;
                        shouldRestartMining = true;
                        
                        NS_LOG_INFO("Node " << nodeId << " completed reorganization from " 
                                   << oldHead << " to " << m_blockchain->getCurrentHead());
                    }
                }
                break;
                
            case BlockRelation::SIBLING:
            case BlockRelation::UNCLE:
                NS_LOG_INFO("Node " << nodeId << " storing uncle/sibling block in DAG");
                m_blockchain->addBlock(receivedBlock);
                break;
                
            case BlockRelation::ORPHAN: {
                NS_LOG_INFO("Node " << nodeId << " received orphan block - storing in DAG");
                m_blockchain->addBlock(receivedBlock);

                auto missingBlocks = m_blockchain->getPendingMissingBlocks();
                if (!missingBlocks.empty() && m_gossipApp) {
                    // Add small delay to missing block requests to allow for natural race conditions
                    double requestDelay = m_networkDelayRNG->GetValue() * 0.5; // 0 to 50% of max network delay
                    Simulator::Schedule(Seconds(1), 
                        [this, missingBlocks]() {
                            if (m_gossipApp) {
                                m_gossipApp->RequestMissingBlocks(missingBlocks);
                            }
                        });
                    NS_LOG_INFO("Node " << nodeId << " will request missing blocks in " << requestDelay << " seconds");
                }
                break;
            }
                
            case BlockRelation::DUPLICATE:
                NS_LOG_DEBUG("Node " << nodeId << " received duplicate block");
                break;
                
            case BlockRelation::REJECTED_UNCLE:
                NS_LOG_INFO("Node " << nodeId << " rejected uncle block due to limits");
                m_blockchain->addBlock(receivedBlock);
                break;
                
            default:
                NS_LOG_WARN("Node " << nodeId << " received invalid block");
                break;
        }
        
        if (chainUpdated) {
            NS_LOG_INFO("Node " << nodeId << " DAG updated. New head: " 
                       << m_blockchain->getCurrentHead() << " Height: " << m_blockchain->getCurrentHeight());
        }
        
        if (shouldRestartMining) {
            restartMining();
        }
    }    
    
    // Handle missing block responses
    void OnMissingBlockResponse(const std::string& blockData) {
        uint32_t nodeId = GetNode()->GetId();
        NS_LOG_INFO("Node " << nodeId << " received missing block response");
        
        // Process like a regular block
        OnBlockReceived(blockData);
        
        // Check if this resolves any pending reorganizations
        checkForPendingReorganizations();
    }
    
private:
    // Generate next mining interval using normal distribution with bounds and variation
    double GetNextMiningInterval() {
        double interval;
        do {
            interval = m_miningIntervalRNG->GetValue();
        } while (interval < s_minMiningInterval || interval > s_maxMiningInterval);
        
        // Add per-mining variation to create realistic timing differences
        double variation = m_miningVariationRNG->GetValue();
        interval = std::max(s_minMiningInterval, interval + variation);
        
        return interval;
    }
    
    void MineBlock() {
        if (!m_running || Simulator::Now().GetSeconds() >= m_stopMiningTime) {
            return;
        }
        
        double currentTime = Simulator::Now().GetSeconds();
        uint32_t nodeId = GetNode()->GetId();
        
        // Record when we started mining this block
        m_currentMiningStartTime = currentTime;
        
        // Always mine on the current chain head (no deliberate stale mining)
        std::string currentHead = m_blockchain->getCurrentHead();
        uint32_t newHeight = m_blockchain->getCurrentHeight() + 1;
        
        // Generate unique block hash
        std::string blockHash = generateBlockHash(newHeight, nodeId, currentTime);
        
        // Create the share/block with proper difficulty
        auto newBlock = std::make_shared<Share>(blockHash, currentHead, newHeight, 
                                              nodeId, currentTime, m_difficulty);
        
        // Add references to recent blocks (DAG feature)
        addDAGReferences(newBlock);
        
        NS_LOG_INFO("Node " << nodeId << " attempting to mine block " << blockHash 
                   << " (height: " << newHeight << ") on parent: " << currentHead);
        
        // Try to add to our DAG
        bool added = m_blockchain->addBlock(newBlock);
        
        if (added) {
            m_blockCounter++;
            totalBlocksMined++;
            perNodeMinedBlocks[nodeId]++;
            
            NS_LOG_INFO("Node " << nodeId << " successfully mined block " << blockHash 
                       << " (height: " << newHeight << ") at time " << currentTime);
            
            // Propagate the block with realistic network delay
            if (m_gossipApp) {
                std::string serializedBlock = newBlock->serialize();
                
                // Add realistic network propagation delay
                double propagationDelay = m_networkDelayRNG->GetValue();
                
                Simulator::Schedule(Seconds(propagationDelay), 
                    [this, serializedBlock, nodeId, propagationDelay]() {
                        if (m_gossipApp) {
                            m_gossipApp->SendBlockMessage(serializedBlock);
                            NS_LOG_DEBUG("Node " << nodeId << " propagated block after " << propagationDelay << "s delay");
                        }
                    });
            }
            
            // Print DAG state periodically
            if (m_blockCounter % 10 == 0) {
                m_blockchain->printDAGState(nodeId);
            }
        } else {
            NS_LOG_DEBUG("Node " << nodeId << " failed to add mined block to DAG");
        }
        
        // Schedule next mining
        ScheduleNextMining();
    }
    
    void ScheduleNextMining() {
        if (!m_running) return;
        
        double nextMiningInterval = GetNextMiningInterval();
        double currentTime = Simulator::Now().GetSeconds();
        
        if (currentTime + nextMiningInterval < m_stopMiningTime) {
            m_miningEvent = Simulator::Schedule(Seconds(nextMiningInterval), &MinerApp::MineBlock, this);
            NS_LOG_DEBUG("Node " << GetNode()->GetId() << " scheduled next mining in " << nextMiningInterval << " seconds");
        }
    }
    
    void restartMining() {
        // Cancel current mining event and start fresh
        if (m_miningEvent.IsRunning()) {
            Simulator::Cancel(m_miningEvent);
        }
        
        m_miningRestarts++;
        uint32_t nodeId = GetNode()->GetId();
        
        // Use variable restart delay to create natural timing differences
        double baseDelay = GetNextMiningInterval() * 0.1; // 10% of normal interval
        double additionalVariation = m_miningVariationRNG->GetValue() * 0.5; // Additional variation
        double restartDelay = std::max(0.1, baseDelay + additionalVariation);
        
        m_miningEvent = Simulator::Schedule(Seconds(restartDelay), &MinerApp::MineBlock, this);
        
        NS_LOG_INFO("Node " << nodeId << " restarting mining on new DAG head " 
                   << m_blockchain->getCurrentHead() << " in " << restartDelay << " seconds");
    }

    // Complete implementation for addDAGReferences function
    void addDAGReferences(std::shared_ptr<Share> block) {
        // Add references to recent blocks for DAG structure
        // This creates a more connected DAG beyond just parent-child relationships
        
        uint32_t currentHeight = m_blockchain->getCurrentHeight();
        uint32_t nodeId = GetNode()->GetId();
        
        // Clear any existing references first
        block->references.clear();
        
        // Strategy 1: Reference recent uncle blocks for better DAG connectivity
        if (currentHeight > 0) {
            // Look at blocks from the same height as parent (uncles/siblings)
            uint32_t parentHeight = currentHeight;
            
            // Get uncle blocks from the current height that are not on main chain
            for (uint32_t height = std::max(0u, parentHeight - 2); height <= parentHeight; height++) {
                auto uncleBlocks = getUncleBlocksAtHeight(height);
                
                // Add up to 2 uncle references per height to maintain performance
                uint32_t referencesAdded = 0;
                for (const auto& uncleHash : uncleBlocks) {
                    if (referencesAdded >= 2) break;
                    
                    // Don't reference our own parent or blocks that are too old
                    if (uncleHash != block->parentHash && uncleHash != "genesis") {
                        block->references.push_back(uncleHash);
                        referencesAdded++;
                        
                        NS_LOG_DEBUG("Node " << nodeId << " added DAG reference from block " 
                                << block->blockHash << " to uncle " << uncleHash);
                    }
                }
            }
        }
        
        // Strategy 2: Reference recent blocks from competing chains for DAG merge
        if (currentHeight > 3) {
            // Look for blocks at recent heights that might represent competing chains
            auto recentCompetingBlocks = getRecentCompetingBlocks(3); // Last 3 heights
            
            uint32_t competingRefsAdded = 0;
            for (const auto& competingHash : recentCompetingBlocks) {
                if (competingRefsAdded >= 1) break; // Limit to 1 competing chain reference
                
                if (std::find(block->references.begin(), block->references.end(), competingHash) == block->references.end()) {
                    block->references.push_back(competingHash);
                    competingRefsAdded++;
                    
                    NS_LOG_DEBUG("Node " << nodeId << " added DAG reference to competing chain block " << competingHash);
                }
            }
        }
        
        // Limit total references to prevent excessive DAG complexity
        if (block->references.size() > 5) {
            block->references.resize(5);
            NS_LOG_DEBUG("Node " << nodeId << " limited DAG references to 5 for block " << block->blockHash);
        }
        
        NS_LOG_INFO("Node " << nodeId << " added " << block->references.size() 
                << " DAG references to block " << block->blockHash);
    }

    // Helper function to get uncle blocks at a specific height
    std::vector<std::string> getUncleBlocksAtHeight(uint32_t height) {
        std::vector<std::string> uncleBlocks;
        
        // Get all blocks at this height
        const auto& blocksAtHeight = m_blockchain->getBlocksByHeight(height);
        
        for (const auto& blockHash : blocksAtHeight) {
            // Check if block is not on main chain (making it an uncle)
            if (!m_blockchain->isOnMainChain(blockHash) && blockHash != "genesis") {
                uncleBlocks.push_back(blockHash);
            }
        }
        
        return uncleBlocks;
    }

    // Helper function to get recent competing chain blocks
    std::vector<std::string> getRecentCompetingBlocks(uint32_t lookbackDepth) {
        std::vector<std::string> competingBlocks;
        uint32_t currentHeight = m_blockchain->getCurrentHeight();
        
        // Look at recent heights for blocks not on main chain
        for (uint32_t height = std::max(0u, currentHeight - lookbackDepth); height < currentHeight; height++) {
            const auto& blocksAtHeight = m_blockchain->getBlocksByHeight(height);
            
            for (const auto& blockHash : blocksAtHeight) {
                if (!m_blockchain->isOnMainChain(blockHash) && blockHash != "genesis") {
                    // This represents a competing chain
                    competingBlocks.push_back(blockHash);
                }
            }
        }
        
        return competingBlocks;
    }

    // Complete implementation for checkForResolvedOrphans function
    void checkForResolvedOrphans() {
        uint32_t nodeId = GetNode()->GetId();
        NS_LOG_DEBUG("Node " << nodeId << " checking for resolved orphan blocks");
        
        // Get current list of missing blocks
        auto missingBlocks = m_blockchain->getMissingBlocks();
        
        if (missingBlocks.empty()) {
            NS_LOG_DEBUG("Node " << nodeId << " no missing blocks to resolve");
            return;
        }
        
        // Track blocks that might now be resolvable
        std::vector<std::string> potentiallyResolved;
        
        // Check each missing block to see if its dependencies are now available
        for (const auto& missingHash : missingBlocks) {
            if (canResolveOrphan(missingHash)) {
                potentiallyResolved.push_back(missingHash);
            }
        }
        
        // Attempt to reprocess potentially resolved orphan blocks
        for (const auto& resolvedHash : potentiallyResolved) {
            NS_LOG_INFO("Node " << nodeId << " attempting to resolve orphan block " << resolvedHash);
            
            // Try to reprocess this block through the DAG
            if (reprocessOrphanBlock(resolvedHash)) {
                NS_LOG_INFO("Node " << nodeId << " successfully resolved orphan block " << resolvedHash);
                
                // This might have resolved other orphans, so trigger chain reaction check
                checkForChainReactionResolution();
            }
        }
        
        NS_LOG_DEBUG("Node " << nodeId << " completed orphan resolution check. Resolved " 
                << potentiallyResolved.size() << " blocks");
    }

    // Helper function to check if an orphan block can now be resolved
    bool canResolveOrphan(const std::string& orphanHash) {
        // Check if this orphan block exists in our DAG
        const BlockDAG& dag = m_blockchain->getDAG();
        const auto& hashToVertex = m_blockchain->getHashToVertexMap();
        
        auto it = hashToVertex.find(orphanHash);
        if (it == hashToVertex.end()) {
            return false; // Block not in DAG
        }
        
        VertexDescriptor orphanVertex = it->second;
        
        // Check if all parent dependencies are now available
        auto inEdges = boost::in_edges(orphanVertex, dag);
        for (auto edgeIt = inEdges.first; edgeIt != inEdges.second; ++edgeIt) {
            if (dag[*edgeIt].edgeType == "parent") {
                VertexDescriptor parentVertex = boost::source(*edgeIt, dag);
                if (!dag[parentVertex].isProcessed) {
                    return false; // Parent still not processed
                }
            }
        }
        
        // Check if all reference dependencies are available (optional for resolution)
        // References are not strictly required for main chain validity
        return true;
    }

    // Helper function to reprocess an orphan block
    bool reprocessOrphanBlock(const std::string& orphanHash) {
        // Get the orphan block from DAG
        const auto& hashToVertex = m_blockchain->getHashToVertexMap();
        auto it = hashToVertex.find(orphanHash);
        
        if (it == hashToVertex.end()) {
            return false;
        }
        
        const BlockDAG& dag = m_blockchain->getDAG();
        VertexDescriptor orphanVertex = it->second;
        std::shared_ptr<Share> orphanBlock = dag[orphanVertex].share;
        
        // Re-analyze the block relationship now that dependencies might be available
        BlockRelation relation = m_blockchain->analyzeBlock(orphanBlock);
        
        // Process based on new relationship
        switch (relation) {
            case BlockRelation::PARENT:
            case BlockRelation::REORG_NEEDED:
                // Block can now extend chain or trigger reorg
                return m_blockchain->performReprocessing(orphanBlock);
                
            case BlockRelation::UNCLE:
            case BlockRelation::SIBLING:
                // Block can now be properly categorized as uncle
                return m_blockchain->updateBlockClassification(orphanHash, relation);
                
            default:
                return false;
        }
    }

    // Helper function to check for chain reaction resolution
    void checkForChainReactionResolution() {
        // Resolving one orphan might make others resolvable
        // Iterate until no more can be resolved
        uint32_t nodeId = GetNode()->GetId();
        uint32_t iterationCount = 0;
        const uint32_t maxIterations = 10; // Prevent infinite loops
        
        bool foundResolvable = true;
        while (foundResolvable && iterationCount < maxIterations) {
            foundResolvable = false;
            iterationCount++;
            
            auto missingBlocks = m_blockchain->getMissingBlocks();
            for (const auto& missingHash : missingBlocks) {
                if (canResolveOrphan(missingHash)) {
                    if (reprocessOrphanBlock(missingHash)) {
                        foundResolvable = true;
                        NS_LOG_DEBUG("Node " << nodeId << " chain reaction resolved " << missingHash);
                        break; // Start over to check all blocks again
                    }
                }
            }
        }
        
        NS_LOG_DEBUG("Node " << nodeId << " completed chain reaction resolution in " 
                << iterationCount << " iterations");
    }

    // Complete implementation for checkForPendingReorganizations function  
    void checkForPendingReorganizations() {
        uint32_t nodeId = GetNode()->GetId();
        NS_LOG_DEBUG("Node " << nodeId << " checking for pending reorganizations");
        
        // Check if any previously incomplete reorganizations can now be completed
        // This happens when missing blocks for reorg paths are now available
        
        const BlockDAG& dag = m_blockchain->getDAG();
        VertexDescriptor currentHeadVertex = m_blockchain->getCurrentHeadVertex();
        
        // Find all leaf nodes (potential alternative chain tips)
        std::vector<VertexDescriptor> leafNodes = findAllLeafNodes();
        
        bool reorgTriggered = false;
        VertexDescriptor bestAlternativeHead;
        uint32_t bestAlternativeWork = m_blockchain->calculatePathWork(currentHeadVertex);
        
        // Evaluate each potential alternative chain
        for (VertexDescriptor leafVertex : leafNodes) {
            if (leafVertex == currentHeadVertex) {
                continue; // Skip current head
            }
            
            // Check if this leaf has heavier work than current head
            uint32_t leafWork = m_blockchain->calculatePathWork(leafVertex);
            
            if (leafWork > bestAlternativeWork) {
                // Check if all blocks in the path to this leaf are now available
                if (isReorgPathComplete(currentHeadVertex, leafVertex)) {
                    bestAlternativeWork = leafWork;
                    bestAlternativeHead = leafVertex;
                    reorgTriggered = true;
                    
                    NS_LOG_INFO("Node " << nodeId << " found heavier alternative chain ending at " 
                            << dag[leafVertex].blockHash << " with work " << leafWork);
                } else {
                    // Request missing blocks for this path
                    requestMissingBlocksForReorg(currentHeadVertex, leafVertex);
                }
            }
        }
        
        // Perform reorganization if we found a complete heavier chain
        if (reorgTriggered) {
            NS_LOG_INFO("Node " << nodeId << " triggering pending reorganization to " 
                    << dag[bestAlternativeHead].blockHash);
            
            std::string oldHead = m_blockchain->getCurrentHead();
            bool success = m_blockchain->performDAGReorganization(dag[bestAlternativeHead].share);
            
            if (success) {
                m_reorgCount++;
                perNodeReorgs[nodeId]++;
                
                NS_LOG_INFO("Node " << nodeId << " completed pending reorganization from " 
                        << oldHead << " to " << m_blockchain->getCurrentHead());
                
                // Restart mining on new head
                restartMining();
            }
        }
        
        NS_LOG_DEBUG("Node " << nodeId << " completed pending reorganization check");
    }

    // Helper function to find all leaf nodes in DAG
    std::vector<VertexDescriptor> findAllLeafNodes() {
        std::vector<VertexDescriptor> leafNodes;
        const BlockDAG& dag = m_blockchain->getDAG();
        
        auto vertexRange = boost::vertices(dag);
        for (auto it = vertexRange.first; it != vertexRange.second; ++it) {
            // A leaf node has no outgoing parent edges (no children)
            bool isLeaf = true;
            auto outEdges = boost::out_edges(*it, dag);
            
            for (auto edgeIt = outEdges.first; edgeIt != outEdges.second; ++edgeIt) {
                if (dag[*edgeIt].edgeType == "parent") {
                    isLeaf = false;
                    break;
                }
            }
            
            if (isLeaf && dag[*it].blockHash != "genesis") {
                leafNodes.push_back(*it);
            }
        }
        
        return leafNodes;
    }

    // Helper function to check if reorg path is complete (no missing blocks)
    bool isReorgPathComplete(VertexDescriptor currentHead, VertexDescriptor targetHead) {
        // Find common ancestor
        VertexDescriptor commonAncestor = m_blockchain->findCommonAncestor(currentHead, targetHead);
        
        // Check if path from common ancestor to target head has any missing blocks
        std::vector<VertexDescriptor> targetPath = m_blockchain->getPathFromAncestor(commonAncestor, targetHead);
        
        for (VertexDescriptor vertex : targetPath) {
            const BlockDAG& dag = m_blockchain->getDAG();
            if (!dag[vertex].isProcessed) {
                return false; // Found unprocessed block in path
            }
            
            // Check if all dependencies of this block are available
            auto inEdges = boost::in_edges(vertex, dag);
            for (auto edgeIt = inEdges.first; edgeIt != inEdges.second; ++edgeIt) {
                VertexDescriptor dependency = boost::source(*edgeIt, dag);
                if (!dag[dependency].isProcessed) {
                    return false; // Dependency not processed
                }
            }
        }
        
        return true;
    }

    // Helper function to request missing blocks for a reorg path
    void requestMissingBlocksForReorg(VertexDescriptor currentHead, VertexDescriptor targetHead) {
        uint32_t nodeId = GetNode()->GetId();
        
        if (!m_gossipApp) {
            return;
        }
        
        // Find blocks missing for this reorg path
        std::vector<std::string> missingBlocks = m_blockchain->getMissingBlocksInPath(currentHead, targetHead);
        
        if (!missingBlocks.empty()) {
            NS_LOG_INFO("Node " << nodeId << " requesting " << missingBlocks.size() 
                    << " missing blocks for potential reorganization");
            
            m_gossipApp->RequestMissingBlocks(missingBlocks);
        }
    }
    
    std::string generateBlockHash(uint32_t height, uint32_t nodeId, double timestamp) {
        std::stringstream ss;
        ss << "block_" << height << "_" << nodeId << "_" << static_cast<uint64_t>(timestamp * 1000);
        return ss.str();
    }
    
    std::string getRelationString(BlockRelation relation) {
        switch (relation) {
            case BlockRelation::PARENT: return "PARENT";
            case BlockRelation::CHILD: return "CHILD";
            case BlockRelation::SIBLING: return "SIBLING";
            case BlockRelation::UNCLE: return "UNCLE";
            case BlockRelation::REORG_NEEDED: return "REORG_NEEDED";
            case BlockRelation::ORPHAN: return "ORPHAN";
            case BlockRelation::DUPLICATE: return "DUPLICATE";
            case BlockRelation::INVALID: return "INVALID";
            case BlockRelation::REJECTED_UNCLE: return "REJECTED_UNCLE";
            default: return "UNKNOWN";
        }
    }
};

// Initialize static members
uint32_t MinerApp::totalBlocksMined = 0;
std::map<uint32_t, uint32_t> MinerApp::perNodeMinedBlocks;
std::map<uint32_t, uint32_t> MinerApp::perNodeReorgs;
std::map<uint32_t, uint32_t> MinerApp::perNodeOrphansReceived;

// Normal distribution parameters for realistic mining intervals
double MinerApp::s_meanMiningInterval = 27.0;    // 15 seconds mean
double MinerApp::s_stdMiningInterval = 4.0;      // 4 seconds standard deviation  
double MinerApp::s_minMiningInterval = 5.0;      // 5 seconds minimum
double MinerApp::s_maxMiningInterval = 35.0;     // 35 seconds maximum

// Network simulation parameters for realistic fork creation
double MinerApp::s_maxNetworkDelay = 0.0;        // Up to 2 seconds network delay
double MinerApp::s_miningVariation = 0.0;        // ±1 second mining variation

void TcpGossipApp::HandleMissingBlockRequest(const std::string& message) {
            uint32_t nodeId = GetNode()->GetId();
            
            // Extract the requested block hash from the message
            // Expected format: "REQUEST_BLOCK|blockHash"
            if (message.find("REQUEST_BLOCK|") == 0) {
                std::string requestedHash = message.substr(14); // Remove "REQUEST_BLOCK|" prefix
                
                NS_LOG_INFO("Node " << nodeId << " received request for block: " << requestedHash);
                
                // Check if we have this block in our blockchain
                if (m_minerApp && m_minerApp->GetBlockchainState()) {
                    const BlockchainState* blockchain = m_minerApp->GetBlockchainState();
                    
                    // Try to find the block in our DAG
                    // Note: You'll need to add a method to BlockchainState to retrieve a block by hash
                    if (blockchain->hasBlock(requestedHash)) {
                        // Get the block data and send it as a response
                        std::string blockData = blockchain->getBlockData(requestedHash);
                        
                        if (!blockData.empty()) {
                            std::string responseMsg = "BLOCK_RESPONSE:" + blockData;
                            
                            NS_LOG_INFO("Node " << nodeId << " sending block response for: " << requestedHash);
                            
                            // Send the response back (you might want to send it only to the requester)
                            ForwardMessage(responseMsg);
                        } else {
                            NS_LOG_WARN("Node " << nodeId << " has block " << requestedHash << " but couldn't retrieve data");
                        }
                    } else {
                        NS_LOG_DEBUG("Node " << nodeId << " doesn't have requested block: " << requestedHash);
                    }
                }
            }
        }

void TcpGossipApp::HandleMissingBlockResponse(const std::string& message) {
            uint32_t nodeId = GetNode()->GetId();
            
            // Extract the block data from the response
            // Expected format: "BLOCK_RESPONSE:blockData"
            if (message.find("BLOCK_RESPONSE:") == 0) {
                std::string blockData = message.substr(15); // Remove "BLOCK_RESPONSE:" prefix
                
                NS_LOG_INFO("Node " << nodeId << " received block response");
                
                // Forward to miner app for processing
                if (m_minerApp) {
                    m_minerApp->OnMissingBlockResponse(blockData);
                }
            }
        }


// Enhanced ProcessReceivedMessage with DAG support and missing block handling
void TcpGossipApp::ProcessReceivedMessage(const std::string& message) {
    uint32_t nodeId = GetNode()->GetId();
    
    // Check if this is a blockchain message
    if (IsBlockchainMessage(message)) {
        // Check if this is a missing block response
        if (message.find("BLOCK_RESPONSE:") == 0) {
            HandleMissingBlockResponse(message);
            return;
        }
        
        // Check if this is a missing block request
        if (message.find("BLOCK_REQUEST:") == 0) {
            HandleMissingBlockRequest(message);
            return;
        }
        
        // Regular block message
        std::string blockHash = ExtractBlockHash(message);
        
        if (!blockHash.empty()) {
            // Check if we've already processed this block
            if (m_messageManager.IsBlockReceived(blockHash)) {
                NS_LOG_DEBUG("Node " << nodeId << " already processed block " << blockHash);
                return; // Already processed
            }
            
            // Mark as received
            m_messageManager.MarkBlockReceived(blockHash);
            
            NS_LOG_INFO("Node " << nodeId << " processing new block " << blockHash);
            
            // Forward to miner app for DAG processing
            if (m_minerApp) {
                m_minerApp->OnBlockReceived(message);
            }
            
            // Forward to neighbors if not already forwarded
            if (!m_messageManager.IsForwarded(message)) {
                m_messageManager.MarkForwarded(message);
                ForwardMessage(message);
                NS_LOG_DEBUG("Node " << nodeId << " forwarded block " << blockHash);
            }
        }
    }
}
    static void exportAllNodesDAG(const std::vector<Ptr<Node>>& nodes, 
                                const std::string& baseFilename, 
                                double timestamp) {
        for (uint32_t i = 0; i < nodes.size(); ++i) {
            Ptr<Application> app = nodes[i]->GetApplication(1);
            Ptr<MinerApp> minerApp = DynamicCast<MinerApp>(app);
            
            if (minerApp) {
                const BlockchainState* blockchain = minerApp->GetBlockchainState();
                if (blockchain) {
                    std::stringstream filename;
                    filename << baseFilename << "_node" << i << "_t" 
                            << std::fixed << std::setprecision(1) << timestamp << ".dot";
                    
                    blockchain->exportDAGToDot(filename.str(), i);
                }
            }
        }
    }

void CreateSmallWorldNetworkP2P(NodeContainer& nodes,
                               PointToPointHelper& pointToPoint,
                               Ipv6AddressHelper& ipv6,
                               std::vector<std::vector<Ipv6Address>>& nodeAddresses,
                               uint32_t numNodes, uint32_t numPeers, 
                               double rewireProbability = 0.5) {
    
    std::cout << "Creating point-to-point small-world network with " << numNodes << " nodes, " 
              << numPeers << " peers per node, and rewire probability " 
              << rewireProbability << std::endl;

    // Initialize node addresses vector
    for (uint32_t i = 0; i < numNodes; i++) {
        nodeAddresses[i].clear();
    }

    // Create neighborhood data structures efficiently
    std::vector<std::unordered_set<uint32_t>> neighbors(numNodes);
    std::set<std::pair<uint32_t, uint32_t>> connections; // To track unique connections

    // Step 1: Create a regular ring lattice
    for (uint32_t i = 0; i < numNodes; i++) {
        for (uint32_t j = 1; j <= numPeers / 2; j++) {
            // Connect to j nodes clockwise
            uint32_t clockwise = (i + j) % numNodes;
            // Connect to j nodes counter-clockwise
            uint32_t counterClockwise = (i - j + numNodes) % numNodes;

            neighbors[i].insert(clockwise);
            neighbors[i].insert(counterClockwise);
            
            // Add to connections set (ensure smaller index comes first to avoid duplicates)
            connections.insert({std::min(i, clockwise), std::max(i, clockwise)});
            connections.insert({std::min(i, counterClockwise), std::max(i, counterClockwise)});
        }
    }

    // Step 2: Rewire some connections with probability p
    std::vector<std::pair<uint32_t, uint32_t>> connectionsToRewire;
    
    for (uint32_t i = 0; i < numNodes; i++) {
        for (uint32_t j = 1; j <= numPeers / 2; j++) {
            uint32_t clockwise = (i + j) % numNodes;
            
            // With probability p, mark this connection for rewiring
            if ((double)rand() / RAND_MAX < rewireProbability) {
                connectionsToRewire.push_back({std::min(i, clockwise), std::max(i, clockwise)});
            }
        }
    }

    // Perform rewiring
    for (auto& connToRewire : connectionsToRewire) {
        uint32_t node1 = connToRewire.first;
        uint32_t node2 = connToRewire.second;
        
        // Remove the original connection
        neighbors[node1].erase(node2);
        neighbors[node2].erase(node1);
        connections.erase(connToRewire);

        // Find a new random connection for node1
        uint32_t attempts = 0;
        uint32_t randomNode;
        bool found = false;

        while (attempts < 100 && !found) {
            randomNode = rand() % numNodes;
            
            // Check it's not the same node and not already connected
            if (randomNode != node1 && neighbors[node1].find(randomNode) == neighbors[node1].end()) {
                found = true;
            }
            attempts++;
        }

        if (found) {
            // Add the new bidirectional connection
            neighbors[node1].insert(randomNode);
            neighbors[randomNode].insert(node1);
            connections.insert({std::min(node1, randomNode), std::max(node1, randomNode)});
        } else {
            // If we couldn't find a suitable new neighbor, keep the original
            neighbors[node1].insert(node2);
            neighbors[node2].insert(node1);
            connections.insert(connToRewire);
        }
    }

    // Now create actual point-to-point connections
    uint32_t subnetCounter = 1;
    
    for (const auto& connection : connections) {
        uint32_t node1 = connection.first;
        uint32_t node2 = connection.second;
        
        // Create point-to-point link between the two nodes
        NodeContainer pair;
        pair.Add(nodes.Get(node1));
        pair.Add(nodes.Get(node2));
        
        NetDeviceContainer devices = pointToPoint.Install(pair);
        
        // Assign IPv6 addresses to this link
        std::ostringstream subnet;
        subnet << "2001:db8:" << std::hex << subnetCounter << "::";
        ipv6.SetBase(Ipv6Address(subnet.str().c_str()), Ipv6Prefix(64));
        
        Ipv6InterfaceContainer interfaces = ipv6.Assign(devices);
        
        // Store the addresses for each node
        Ipv6Address addr1 = interfaces.GetAddress(0, 1); // node1's address on this link
        Ipv6Address addr2 = interfaces.GetAddress(1, 1); // node2's address on this link
        
        nodeAddresses[node1].push_back(addr1);
        nodeAddresses[node2].push_back(addr2);
        
        subnetCounter++;
    }
    
    // Calculate and display average node degree for verification
    double avgDegree = 0.0;
    for (uint32_t i = 0; i < numNodes; i++) {
        avgDegree += neighbors[i].size();
    }
    avgDegree /= numNodes;
    
    std::cout << "Point-to-point small-world network topology created successfully" << std::endl;
    std::cout << "Total P2P connections created: " << connections.size() << std::endl;
    std::cout << "Average node degree: " << avgDegree << std::endl;
}

// Helper function to check if two IPv6 addresses are on the same subnet
bool AreSameSubnet(const Ipv6Address& addr1, const Ipv6Address& addr2) {
    // Get the bytes of both addresses
    uint8_t bytes1[16], bytes2[16];
    addr1.GetBytes(bytes1);
    addr2.GetBytes(bytes2);
    
    // Compare first 8 bytes (64-bit prefix)
    for (int i = 0; i < 8; i++) {
        if (bytes1[i] != bytes2[i]) {
            return false;
        }
    }
    return true;
}

void AddNeighborsToGossipApps(std::vector<Ptr<TcpGossipApp>>& gossipApps,
    const std::vector<std::vector<Ipv6Address>>& nodeAddresses,
    uint32_t numNodes) {
        
        std::cout << "Adding neighbors to gossip applications..." << std::endl;
        
        // We need to reconstruct the neighbor relationships from the network topology
        // Since we know the addresses were added in pairs during P2P connection creation,
        // we need to find which nodes are connected to which other nodes
        
        // Create a mapping from address to node index
        std::map<Ipv6Address, uint32_t> addressToNode;
        std::map<Ipv6Address, uint32_t> addressToInterface; // which interface index on that node
        
        for (uint32_t i = 0; i < numNodes; i++) {
        for (uint32_t j = 0; j < nodeAddresses[i].size(); j++) {
            addressToNode[nodeAddresses[i][j]] = i;
            addressToInterface[nodeAddresses[i][j]] = j;
        }
    }
    
    // For each node, find its neighbors by looking at the network connections
    // Since addresses were added in pairs during connection creation,
    // we need to identify which addresses belong to the same subnet
    for (uint32_t nodeA = 0; nodeA < numNodes; nodeA++) {
        for (uint32_t interfaceA = 0; interfaceA < nodeAddresses[nodeA].size(); interfaceA++) {
            Ipv6Address addrA = nodeAddresses[nodeA][interfaceA];
            
            // Find the corresponding address on the same subnet
            for (uint32_t nodeB = 0; nodeB < numNodes; nodeB++) {
                if (nodeB == nodeA) continue; // Skip self
                
                for (uint32_t interfaceB = 0; interfaceB < nodeAddresses[nodeB].size(); interfaceB++) {
                    Ipv6Address addrB = nodeAddresses[nodeB][interfaceB];
                    
                    // Check if these addresses are on the same subnet
                    // They should have the same first 64 bits (network prefix)
                    if (AreSameSubnet(addrA, addrB)) {
                        // These nodes are connected - add as neighbors
                        gossipApps[nodeA]->AddNeighbor(addrB);
                        // Note: We don't add the reverse here because it will be handled
                        // when we process nodeB's interfaces
                        break; // Found the partner for this interface
                    }
                }
            }
        }
    }
    
    std::cout << "Neighbors added to gossip applications successfully" << std::endl;
}


// Add this function before your main() function
void PrintSimulationProgress(double interval) {
    std::cout << "Simulation time passed: " << Simulator::Now().GetSeconds() << " secs" << std::endl;
    
    // Schedule the next progress update
    Simulator::Schedule(Seconds(interval), &PrintSimulationProgress, interval);
}


// Add this class before the main() function
class NetworkMonitor {
    private:
        std::vector<Ptr<TcpGossipApp>>& m_gossipApps;
        std::vector<Ptr<MinerApp>>& m_minerApps;
        uint32_t m_numNodes;
        EventId m_reportEvent;
        double m_reportInterval;
        
        // Store historical data for reporting
        struct ReportData {
            double timestamp;
            uint32_t totalBlocksMined;
            uint32_t totalBlocksReceived;
            double avgBlocksPropagated;
            uint32_t minBlocksReceived;
            uint32_t maxBlocksReceived;
        };
        
        std::vector<ReportData> m_reportHistory;
        
    public:
        NetworkMonitor(std::vector<Ptr<TcpGossipApp>>& gossipApps,
                       std::vector<Ptr<MinerApp>>& minerApps,
                       double reportInterval)
            : m_gossipApps(gossipApps), 
              m_minerApps(minerApps),
              m_reportInterval(reportInterval) 
        {
            m_numNodes = gossipApps.size();
        }
        
        void Start() {
            // Schedule the first report
            m_reportEvent = Simulator::Schedule(Seconds(m_reportInterval), 
                                               &NetworkMonitor::GenerateReport, 
                                               this);
        }
        
        void Stop() {
            if (m_reportEvent.IsRunning()) {
                Simulator::Cancel(m_reportEvent);
            }
            
            // Generate final summary
            PrintSummary();
        }
        
    private:
        void GenerateReport() {
            double currentTime = Simulator::Now().GetSeconds();
            uint32_t totalBlocksMined = MinerApp::totalBlocksMined;
            
            // Calculate block propagation statistics
            uint32_t totalReceivedBlocks = 0;
            uint32_t minReceivedBlocks = UINT32_MAX;
            uint32_t maxReceivedBlocks = 0;
            
            for (uint32_t i = 0; i < m_numNodes; i++) {
                uint32_t receivedBlocks = m_gossipApps[i]->GetReceivedBlockCount();
                totalReceivedBlocks += receivedBlocks;
                minReceivedBlocks = std::min(minReceivedBlocks, receivedBlocks);
                maxReceivedBlocks = std::max(maxReceivedBlocks, receivedBlocks);
            }
            
            double avgReceivedBlocks = static_cast<double>(totalReceivedBlocks) / m_numNodes;
            double propagationRatio = (totalBlocksMined > 0) ? 
                                     (avgReceivedBlocks / totalBlocksMined) * 100.0 : 0.0;
            
            // Store the report data
            ReportData report;
            report.timestamp = currentTime;
            report.totalBlocksMined = totalBlocksMined;
            report.totalBlocksReceived = totalReceivedBlocks;
            report.avgBlocksPropagated = avgReceivedBlocks;
            report.minBlocksReceived = minReceivedBlocks;
            report.maxBlocksReceived = maxReceivedBlocks;
            
            m_reportHistory.push_back(report);
            
            // Print the current report
            std::cout << "\n=== NETWORK REPORT AT " << std::setprecision(1) << currentTime << " SECONDS ===" << std::endl;
            std::cout << "Total blocks mined: " << totalBlocksMined << std::endl;
            std::cout << "Block propagation:" << std::endl;
            std::cout << "  Average blocks received per node: " << std::setprecision(2) << avgReceivedBlocks << std::endl;
            std::cout << "  Propagation efficiency: " << std::setprecision(2) << propagationRatio << "%" << std::endl;
            std::cout << "  Min blocks received: " << minReceivedBlocks << std::endl;
            std::cout << "  Max blocks received: " << maxReceivedBlocks << std::endl;
            std::cout << "=================================================" << std::endl;
            
            // Schedule the next report
            m_reportEvent = Simulator::Schedule(Seconds(m_reportInterval), 
                                               &NetworkMonitor::GenerateReport, 
                                               this);
        }
        
        void PrintSummary() {
            if (m_reportHistory.empty()) return;
            
            std::cout << "\n\n========== SUMMARY OF NETWORK REPORTS ==========" << std::endl;
            std::cout << "Time(s)\tBlocks Mined\tAvg Blocks Received\tPropagation %" << std::endl;
            
            for (const auto& report : m_reportHistory) {
                double propagationRatio = (report.totalBlocksMined > 0) ? 
                                         (report.avgBlocksPropagated / report.totalBlocksMined) * 100.0 : 0.0;
                                         
                std::cout << std::setprecision(1) << report.timestamp << "\t"
                          << report.totalBlocksMined << "\t\t"
                          << std::setprecision(2) << report.avgBlocksPropagated << "\t\t\t"
                          << std::setprecision(2) << propagationRatio << "%" << std::endl;
            }
            
            std::cout << "=================================================" << std::endl;
        }
    };
 void AnalyzeNetworkConsensus(const std::vector<Ptr<Node>>& nodes, uint32_t numNodes) {
    NS_LOG_INFO("################################################################################");
    NS_LOG_INFO("                    ADVANCED DAG BLOCKCHAIN NETWORK ANALYSIS");
    NS_LOG_INFO("################################################################################");
    
    std::map<std::string, std::vector<uint32_t>> consensusGroups;
    std::map<uint32_t, std::vector<uint32_t>> heightGroups;
    std::map<uint32_t, std::vector<uint32_t>> uncleCountGroups;
    std::map<uint32_t, std::vector<uint32_t>> orphanCountGroups;
    
    // Collect data from all nodes
    std::vector<BlockchainState::ChainAnalysis> nodeAnalyses;
    std::vector<BlockchainState::NetworkHealth> healthMetrics;
    
    for (uint32_t i = 0; i < numNodes; ++i) {
        Ptr<Node> node = nodes[i];
        if (node->GetNApplications() > 1) {
            Ptr<Application> app = node->GetApplication(1);
            Ptr<MinerApp> minerApp = DynamicCast<MinerApp>(app);
            
            if (minerApp) {
                const BlockchainState* blockchain = minerApp->GetBlockchainState();
                if (blockchain) {
                    std::string head = blockchain->getCurrentHead();
                    uint32_t height = blockchain->getCurrentHeight();
                    
                    consensusGroups[head].push_back(i);
                    heightGroups[height].push_back(i);
                    
                    // Get detailed analysis
                    auto analysis = blockchain->getDetailedAnalysis();
                    nodeAnalyses.push_back(analysis);
                    
                    auto health = blockchain->getNetworkHealth();
                    healthMetrics.push_back(health);
                    
                    // Group by uncle and orphan counts
                    uncleCountGroups[analysis.uncleBlocks].push_back(i);
                    orphanCountGroups[analysis.orphanBlocks].push_back(i);
                }
            }
        }
    }
    
    if (consensusGroups.empty()) {
        NS_LOG_INFO("⚠ ERROR: No valid blockchain states found!");
        return;
    }
    
    // Consensus Analysis
    NS_LOG_INFO("\n=== CONSENSUS ANALYSIS ===");
    NS_LOG_INFO("Number of different chain heads: " << consensusGroups.size());
    
    if (consensusGroups.size() == 1) {
        NS_LOG_INFO("✓ PERFECT CONSENSUS: All nodes agree on the same chain head!");
    } else {
        NS_LOG_INFO("⚠ FORK DETECTED: Network has split into " << consensusGroups.size() << " groups:");
        
        for (const auto& group : consensusGroups) {
            double percentage = (100.0 * group.second.size()) / numNodes;
            NS_LOG_INFO("  Chain head " << group.first << ": " 
                    << group.second.size() << " nodes (" 
                    << std::fixed << std::setprecision(1) << percentage << "%)");
        }
    }
    
    // Height Analysis
    NS_LOG_INFO("\n=== CHAIN HEIGHT ANALYSIS ===");
    for (const auto& group : heightGroups) {
        double percentage = (100.0 * group.second.size()) / numNodes;
        NS_LOG_INFO("  Height " << group.first << ": " 
                << group.second.size() << " nodes (" 
                << std::fixed << std::setprecision(1) << percentage << "%)");
    }
    
    // Uncle Block Analysis
    NS_LOG_INFO("\n=== UNCLE BLOCK ANALYSIS ===");
    for (const auto& group : uncleCountGroups) {
        double percentage = (100.0 * group.second.size()) / numNodes;
        NS_LOG_INFO("  " << group.first << " uncles: " 
                << group.second.size() << " nodes (" 
                << std::fixed << std::setprecision(1) << percentage << "%)");
    }
    
    // Orphan Block Analysis
    NS_LOG_INFO("\n=== ORPHAN BLOCK ANALYSIS ===");
    for (const auto& group : orphanCountGroups) {
        double percentage = (100.0 * group.second.size()) / numNodes;
        NS_LOG_INFO("  " << group.first << " orphans: " 
                << group.second.size() << " nodes (" 
                << std::fixed << std::setprecision(1) << percentage << "%)");
    }
    
    // Network Health Summary
    NS_LOG_INFO("\n=== NETWORK HEALTH SUMMARY ===");
    if (!healthMetrics.empty()) {
        double avgOrphanRate = 0.0;
        double avgUncleEfficiency = 0.0;
        uint32_t healthyNodes = 0;
        
        for (const auto& health : healthMetrics) {
            avgOrphanRate += health.orphanRate;
            avgUncleEfficiency += health.uncleEfficiency;
            if (health.isHealthy) healthyNodes++;
        }
        
        avgOrphanRate /= healthMetrics.size();
        avgUncleEfficiency /= healthMetrics.size();
        
        NS_LOG_INFO("  Average orphan rate: " << std::fixed << std::setprecision(3) << (avgOrphanRate * 100) << "%");
        NS_LOG_INFO("  Average uncle efficiency: " << std::fixed << std::setprecision(3) << (avgUncleEfficiency * 100) << "%");
        NS_LOG_INFO("  Healthy nodes: " << healthyNodes << "/" << numNodes << " (" 
                   << std::fixed << std::setprecision(1) << (100.0 * healthyNodes / numNodes) << "%)");
        
        if (healthyNodes >= numNodes * 0.8) {
            NS_LOG_INFO("  ✓ NETWORK STATUS: HEALTHY");
        } else if (healthyNodes >= numNodes * 0.5) {
            NS_LOG_INFO("  ⚠ NETWORK STATUS: DEGRADED");
        } else {
            NS_LOG_INFO("  ✗ NETWORK STATUS: UNHEALTHY");
        }
    }
    
    NS_LOG_INFO("################################################################################");
}

void PrintDetailedBlockchainAnalysis(const std::vector<Ptr<Node>>& nodes, 
                                   const std::vector<uint32_t>& nodeIds) {
    NS_LOG_INFO("\n");
    NS_LOG_INFO("################################################################################");
    NS_LOG_INFO("                    DETAILED DAG BLOCKCHAIN ANALYSIS");
    NS_LOG_INFO("################################################################################");
    
    for (uint32_t nodeId : nodeIds) {
        if (nodeId < nodes.size()) {
            Ptr<Node> node = nodes[nodeId];
            
            if (node->GetNApplications() > 1) {
                Ptr<Application> app = node->GetApplication(1);
                Ptr<MinerApp> minerApp = DynamicCast<MinerApp>(app);
                
                if (minerApp) {
                    const BlockchainState* blockchain = minerApp->GetBlockchainState();
                    if (blockchain) {
                        // Print complete state
                        blockchain->printCompleteState(nodeId);
                        
                        // Print detailed analysis
                        auto analysis = blockchain->getDetailedAnalysis();
                        NS_LOG_INFO("--- DETAILED ANALYSIS FOR NODE " << nodeId << " ---");
                        NS_LOG_INFO("  Total Blocks: " << analysis.totalBlocks);
                        NS_LOG_INFO("  Main Chain Length: " << analysis.mainChainLength);
                        NS_LOG_INFO("  Side Blocks: " << analysis.sideBlocks);
                        NS_LOG_INFO("  Uncle Blocks: " << analysis.uncleBlocks);
                        NS_LOG_INFO("  Orphan Blocks: " << analysis.orphanBlocks);
                        NS_LOG_INFO("  Unique Miners: " << analysis.uniqueMiners);
                        NS_LOG_INFO("  Average Block Time: " << std::fixed << std::setprecision(2) 
                                   << analysis.averageBlockTime << "s");
                        
                        // Print miner distribution
                        NS_LOG_INFO("  Miner Distribution:");
                        for (const auto& pair : analysis.minerDistribution) {
                            double percentage = (100.0 * pair.second) / analysis.totalBlocks;
                            NS_LOG_INFO("    Miner " << pair.first << ": " << pair.second 
                                       << " blocks (" << std::fixed << std::setprecision(1) 
                                       << percentage << "%)");
                        }
                        
                        // Print fork analysis
                        auto forkInfo = blockchain->getForkAnalysis();
                        NS_LOG_INFO("  Fork Analysis:");
                        NS_LOG_INFO("    Fork Points: " << forkInfo.forkPoints.size());
                        NS_LOG_INFO("    Max Fork Depth: " << forkInfo.maxForkDepth);
                        NS_LOG_INFO("    Fork Ratio: " << std::fixed << std::setprecision(3) 
                                   << (forkInfo.forkRatio * 100) << "%");
                        
                        // Print network health
                        auto health = blockchain->getNetworkHealth();
                        NS_LOG_INFO("  Network Health:");
                        NS_LOG_INFO("    Orphan Rate: " << std::fixed << std::setprecision(3) 
                                   << (health.orphanRate * 100) << "%");
                        NS_LOG_INFO("    Uncle Efficiency: " << std::fixed << std::setprecision(3) 
                                   << (health.uncleEfficiency * 100) << "%");
                        NS_LOG_INFO("    Status: " << (health.isHealthy ? "HEALTHY" : "DEGRADED"));
                        
                        NS_LOG_INFO("");
                    }
                }
            }
        }
    }
    
    NS_LOG_INFO("################################################################################");
}

void CompareDAGBlockchainStates(const std::vector<Ptr<Node>>& nodes, 
                               const std::vector<uint32_t>& nodeIds) {
    NS_LOG_INFO("\n");
    NS_LOG_INFO("################################################################################");
    NS_LOG_INFO("                    DAG BLOCKCHAIN STATES COMPARISON");
    NS_LOG_INFO("################################################################################");
    
    std::vector<BlockchainState::ChainAnalysis> analyses;
    std::map<std::string, std::vector<uint32_t>> headGroups;
    std::map<uint32_t, std::vector<uint32_t>> heightGroups;
    
    // Collect data
    for (uint32_t nodeId : nodeIds) {
        if (nodeId < nodes.size()) {
            Ptr<Node> node = nodes[nodeId];
            if (node->GetNApplications() > 1) {
                Ptr<Application> app = node->GetApplication(1);
                Ptr<MinerApp> minerApp = DynamicCast<MinerApp>(app);
                
                if (minerApp) {
                    const BlockchainState* blockchain = minerApp->GetBlockchainState();
                    if (blockchain) {
                        std::string head = blockchain->getCurrentHead();
                        uint32_t height = blockchain->getCurrentHeight();
                        
                        headGroups[head].push_back(nodeId);
                        heightGroups[height].push_back(nodeId);
                        
                        auto analysis = blockchain->getDetailedAnalysis();
                        analyses.push_back(analysis);
                        
                        NS_LOG_INFO("Node " << nodeId << " - Head: " << head 
                                   << ", Height: " << height 
                                   << ", Uncles: " << analysis.uncleBlocks 
                                   << ", Orphans: " << analysis.orphanBlocks);
                    }
                }
            }
        }
    }
    
    // Summary statistics
    if (!analyses.empty()) {
        NS_LOG_INFO("\n=== COMPARISON SUMMARY ===");
        
        // Calculate averages
        double avgTotalBlocks = 0, avgUncles = 0, avgOrphans = 0;
        uint32_t minHeight = UINT32_MAX, maxHeight = 0;
        
        for (const auto& analysis : analyses) {
            avgTotalBlocks += analysis.totalBlocks;
            avgUncles += analysis.uncleBlocks;
            avgOrphans += analysis.orphanBlocks;
            
            minHeight = std::min(minHeight, analysis.mainChainLength - 1);
            maxHeight = std::max(maxHeight, analysis.mainChainLength - 1);
        }
        
        avgTotalBlocks /= analyses.size();
        avgUncles /= analyses.size();
        avgOrphans /= analyses.size();
        
        NS_LOG_INFO("Average total blocks: " << std::fixed << std::setprecision(1) << avgTotalBlocks);
        NS_LOG_INFO("Average uncle blocks: " << std::fixed << std::setprecision(1) << avgUncles);
        NS_LOG_INFO("Average orphan blocks: " << std::fixed << std::setprecision(1) << avgOrphans);
        NS_LOG_INFO("Height range: " << minHeight << " - " << maxHeight 
                   << " (spread: " << (maxHeight - minHeight) << ")");
        
        // Consensus analysis
        NS_LOG_INFO("\nConsensus Groups:");
        NS_LOG_INFO("Different chain heads: " << headGroups.size());
        for (const auto& pair : headGroups) {
            NS_LOG_INFO("  Head " << pair.first << ": " << pair.second.size() << " nodes");
        }
        
        NS_LOG_INFO("\nHeight Distribution:");
        for (const auto& pair : heightGroups) {
            NS_LOG_INFO("  Height " << pair.first << ": " << pair.second.size() << " nodes");
        }
        
        // Determine network state
        if (headGroups.size() == 1) {
            NS_LOG_INFO("\n✓ CONSENSUS ACHIEVED: All nodes have same chain head");
        } else {
            NS_LOG_INFO("\n⚠ FORK PRESENT: " << headGroups.size() << " different chain heads");
        }
        
        if (maxHeight - minHeight <= 1) {
            NS_LOG_INFO("✓ HEIGHT SYNC: All nodes within 1 block of each other");
        } else {
            NS_LOG_INFO("⚠ HEIGHT DIVERGENCE: " << (maxHeight - minHeight) << " block spread");
        }
    }
    
    NS_LOG_INFO("################################################################################\n");
}

// Updated random node selection with better distribution
std::vector<uint32_t> SelectRandomNodesAdvanced(uint32_t totalNodes, uint32_t numToSelect, 
                                               bool includeExtremes = true) {
    std::vector<uint32_t> selected;
    
    if (numToSelect >= totalNodes) {
        // Return all nodes
        for (uint32_t i = 0; i < totalNodes; ++i) {
            selected.push_back(i);
        }
        return selected;
    }
    
    std::random_device rd;
    std::mt19937 gen(rd());
    
    if (includeExtremes && totalNodes > 2) {
        // Always include first and last node for comparison
        selected.push_back(0);
        selected.push_back(totalNodes - 1);
        numToSelect -= 2;
    }
    
    // Select remaining nodes randomly
    std::vector<uint32_t> remaining;
    uint32_t start = includeExtremes ? 1 : 0;
    uint32_t end = includeExtremes ? totalNodes - 1 : totalNodes;
    
    for (uint32_t i = start; i < end; ++i) {
        remaining.push_back(i);
    }
    
    std::shuffle(remaining.begin(), remaining.end(), gen);
    
    for (uint32_t i = 0; i < std::min(numToSelect, (uint32_t)remaining.size()); ++i) {
        selected.push_back(remaining[i]);
    }
    
    std::sort(selected.begin(), selected.end());
    return selected;
}
// Option 1: Clean directory at simulation start
void CleanOutputDirectory() {
    NS_LOG_INFO("Cleaning output directory: " << outputDir);
    
    // Remove existing directory and recreate
    std::string rmCmd = "rm -rf " + outputDir;
    std::string mkdirCmd = "mkdir -p " + outputDir;
    
    system(rmCmd.c_str());
    system(mkdirCmd.c_str());
    
    NS_LOG_INFO("Output directory cleaned and recreated");
}

// Option 2: Clean with timestamp-based directory
void InitializeOutputDirectory() {
    // Create timestamped directory
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);
    
    std::stringstream timestamp;
    timestamp << std::put_time(&tm, "%Y%m%d_%H%M%S");
    
    outputDir = "dag_results_" + timestamp.str() + "/";
    
    std::string mkdirCmd = "mkdir -p " + outputDir;
    system(mkdirCmd.c_str());
    
    NS_LOG_INFO("Created timestamped output directory: " << outputDir);
}

// Modified ExportDAGSnapshots with cleanup check
void ExportDAGSnapshots(double exportTime) {
    NS_LOG_INFO("Exporting DAG snapshots at time " << exportTime);
    
    // Only create directory if it doesn't exist (don't clean here)
    std::string mkdirCmd = "mkdir -p " + outputDir;
    system(mkdirCmd.c_str());
    
    // Export DAG for each node
    for (uint32_t i = 0; i < nodes.GetN(); ++i) {
        Ptr<Application> app = nodes.Get(i)->GetApplication(1);
        Ptr<MinerApp> minerApp = DynamicCast<MinerApp>(app);
        
        if (minerApp) {
            const BlockchainState* blockchain = minerApp->GetBlockchainState();
            if (blockchain) {
                std::stringstream filename;
                filename << outputDir << "blockchain_dag_node" << i 
                        << "_t" << std::fixed << std::setprecision(1) << exportTime << ".dot";

                blockchain->exportDAGToDot(filename.str(), i);
            }
        }
    }
}

// Option 3: Clean specific file patterns
void CleanOldDAGFiles() {
    NS_LOG_INFO("Cleaning old DAG files from: " << outputDir);
    
    // Remove old DAG files
    std::string cleanCmd = "rm -f " + outputDir + "blockchain_dag_*.dot";
    system(cleanCmd.c_str());
    
    // Remove old analysis files
    cleanCmd = "rm -f " + outputDir + "analysis_*.txt";
    system(cleanCmd.c_str());
    
    // Remove old images
    cleanCmd = "rm -rf " + outputDir + "images/";
    system(cleanCmd.c_str());
    
    // Remove old final summary
    cleanCmd = "rm -rf " + outputDir + "final_summary/";
    system(cleanCmd.c_str());
    
    NS_LOG_INFO("Old DAG files cleaned");
}

// Enhanced initialization function
void InitializeDAGExports(bool cleanExisting = true) {
    if (cleanExisting) {
        CleanOutputDirectory();  // Option 1: Complete cleanup
        // OR
        // CleanOldDAGFiles();   // Option 3: Selective cleanup
    } else {
        InitializeOutputDirectory();  // Option 2: Timestamped directory
    }
    
    NS_LOG_INFO("DAG export system initialized");
}

// Function to schedule periodic DAG exports (unchanged)
void ScheduleDAGExports(double startTime, double interval, double endTime) {
    for (double t = startTime; t <= endTime; t += interval) {
        Simulator::Schedule(Seconds(t), &ExportDAGSnapshots, t);
    }
}


// Your existing functions remain the same...
void exportDetailedAnalysis(const BlockchainState& blockchain, 
                           const std::string& filename, 
                           uint32_t nodeId) {
    std::ofstream analysisFile(filename);
    if (!analysisFile.is_open()) {
        NS_LOG_ERROR("Failed to open analysis file: " << filename);
        return;
    }
    
    auto analysis = blockchain.getDetailedAnalysis();
    auto forkInfo = blockchain.getForkAnalysis();
    auto networkHealth = blockchain.getNetworkHealth();
    
    analysisFile << "=== BLOCKCHAIN ANALYSIS - NODE " << nodeId << " ===\n\n";
    
    analysisFile << "BASIC METRICS:\n";
    analysisFile << "  Total Blocks: " << analysis.totalBlocks << "\n";
    analysisFile << "  Main Chain Length: " << analysis.mainChainLength << "\n";
    analysisFile << "  Side Blocks: " << analysis.sideBlocks << "\n";
    analysisFile << "  Uncle Blocks: " << analysis.uncleBlocks << "\n";
    analysisFile << "  Orphan Blocks: " << analysis.orphanBlocks << "\n";
    analysisFile << "  Unique Miners: " << analysis.uniqueMiners << "\n";
    analysisFile << "  Average Block Time: " << analysis.averageBlockTime << " seconds\n\n";
    
    analysisFile << "FORK ANALYSIS:\n";
    analysisFile << "  Fork Points: " << forkInfo.forkPoints.size() << "\n";
    analysisFile << "  Max Fork Depth: " << forkInfo.maxForkDepth << "\n";
    analysisFile << "  Fork Ratio: " << (forkInfo.forkRatio * 100) << "%\n\n";
    
    analysisFile << "NETWORK HEALTH:\n";
    analysisFile << "  Orphan Rate: " << (networkHealth.orphanRate * 100) << "%\n";
    analysisFile << "  Uncle Efficiency: " << (networkHealth.uncleEfficiency * 100) << "%\n";
    analysisFile << "  Overall Health: " << (networkHealth.isHealthy ? "HEALTHY" : "UNHEALTHY") << "\n\n";
    
    analysisFile << "MINER DISTRIBUTION:\n";
    for (const auto& pair : analysis.minerDistribution) {
        analysisFile << "  Miner " << pair.first << ": " << pair.second << " blocks\n";
    }
    
    analysisFile.close();
    NS_LOG_INFO("Analysis exported to: " << filename);
}

void generateVisualizationScript() {
    std::string scriptFilename = outputDir + "visualize_all.sh";
    std::ofstream scriptFile(scriptFilename);
    
    if (!scriptFile.is_open()) {
        NS_LOG_ERROR("Failed to create visualization script");
        return;
    }
    
    scriptFile << "#!/bin/bash\n\n";
    scriptFile << "# Script to generate PNG images from DOT files\n\n";
    scriptFile << "echo \"Converting DOT files to PNG images...\"\n\n";
    
    scriptFile << "# Create images directory\n";
    scriptFile << "mkdir -p " << outputDir << "images/\n\n";
    
    scriptFile << "# Convert all DOT files to PNG\n";
    scriptFile << "for dotfile in " << outputDir << "*.dot; do\n";
    scriptFile << "    if [ -f \"$dotfile\" ]; then\n";
    scriptFile << "        filename=$(basename \"$dotfile\" .dot)\n";
    scriptFile << "        echo \"Converting $filename.dot to PNG...\"\n";
    scriptFile << "        dot -Tpng \"$dotfile\" -o \"" << outputDir << "images/${filename}.png\"\n";
    scriptFile << "    fi\n";
    scriptFile << "done\n\n";
    
    scriptFile << "# Convert final summary DOT files\n";
    scriptFile << "for dotfile in " << outputDir << "final_summary/*.dot; do\n";
    scriptFile << "    if [ -f \"$dotfile\" ]; then\n";
    scriptFile << "        filename=$(basename \"$dotfile\" .dot)\n";
    scriptFile << "        echo \"Converting final summary $filename.dot to PNG...\"\n";
    scriptFile << "        dot -Tpng \"$dotfile\" -o \"" << outputDir << "images/final_${filename}.png\"\n";
    scriptFile << "    fi\n";
    scriptFile << "done\n\n";
    
    scriptFile << "echo \"Visualization complete! Check the images/ directory.\"\n";
    scriptFile << "echo \"You can also generate SVG files with: dot -Tsvg input.dot -o output.svg\"\n";
    
    scriptFile.close();
    
    // Make script executable
    std::string chmodCmd = "chmod +x " + scriptFilename;
    system(chmodCmd.c_str());
    
    NS_LOG_INFO("Visualization script created: " << scriptFilename);
}

// Enhanced final export with better cleanup
void ExportFinalDAGState() {
    NS_LOG_INFO("Exporting final DAG state");
    
    double finalTime = Simulator::Now().GetSeconds();
    
    // Create summary directory
    std::string summaryDir = outputDir + "final_summary/";
    std::string mkdirCmd = "mkdir -p " + summaryDir;
    system(mkdirCmd.c_str());
    
    // Export final state for each node
    for (uint32_t i = 0; i < nodes.GetN(); ++i) {
        Ptr<Application> app = nodes.Get(i)->GetApplication(1);
        Ptr<MinerApp> minerApp = DynamicCast<MinerApp>(app);
        
        if (minerApp) {
            const BlockchainState* blockchain = minerApp->GetBlockchainState();
            if (blockchain) {
                // Export final DAG
                std::string filename = summaryDir + "final_dag_node" + std::to_string(i) + ".dot";
                blockchain->exportDAGToDot(filename, i);
                
                // Export detailed analysis
                std::string analysisFilename = summaryDir + "analysis_node" + std::to_string(i) + ".txt";
                exportDetailedAnalysis(*blockchain, analysisFilename, i);
            }
        }
    }
    
    // Generate visualization script
    generateVisualizationScript();
    
    NS_LOG_INFO("Final DAG export completed to: " << outputDir);
}


int main(int argc, char* argv[]) {

    auto start_time = std::chrono::high_resolution_clock::now();
    
    CommandLine cmd;
    bool enableDAGExport = true;
    double dagExportInterval = 30.0;
    std::string dagOutputDir = "dag_output/";
    bool cleanDAGOutput = true;  // Add this option
    
    cmd.AddValue("enableDAGExport", "Enable DAG DOT file export", enableDAGExport);
    cmd.AddValue("dagExportInterval", "Interval between DAG exports (seconds)", dagExportInterval);
    cmd.AddValue("dagOutputDir", "Directory for DAG output files", dagOutputDir);
    cmd.AddValue("cleanDAGOutput", "Clean output directory before simulation", cleanDAGOutput);
    cmd.Parse(argc, argv);
    
    // Set global output directory
    outputDir = dagOutputDir;
    
    // Initialize DAG export system (this will clean the directory)
    if (enableDAGExport) {
        InitializeDAGExports(cleanDAGOutput);
    }

    // Seed the random number generator with current time
    srand(time(nullptr));
    uint32_t numNodes = 500;
    uint32_t numPeers = 10;  // Changed to 8 connections per node
    double rewireProbability = 0.5;
    double simulationTime = 100.0;

    BlockchainState blockchain(2);  // Max 5 uncles per height

    // Force decimal point display to avoid locale issues
    std::cout.setf(std::ios_base::fixed, std::ios_base::floatfield);

    // Print simulation parameters
    std::cout << "Starting TCP Gossip simulation with:" << std::endl;
    std::cout << "  Number of nodes: " << numNodes << std::endl;
    std::cout << "  Peers per node: " << numPeers << std::endl;
    std::cout << "  Rewire probability: " << rewireProbability << std::endl;
    std::cout << "  Simulation time: " << simulationTime << " seconds" << std::endl;

    // Set up simulation environment
    Time::SetResolution(Time::NS);
    LogComponentEnable("TcpGossip", LOG_LEVEL_INFO);

    // Create nodes
    // NodeContainer nodes;
    nodes.Create(numNodes);

    // Set up internet stack
    InternetStackHelper internet;
    internet.Install(nodes);

    // Create point-to-point helper
    PointToPointHelper pointToPoint;
    pointToPoint.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    pointToPoint.SetChannelAttribute("Delay", TimeValue(MilliSeconds(300))); // 300ms delay


    // Create IPv6 address helper
    Ipv6AddressHelper ipv6;
    
    // Store all interfaces for each node
    std::vector<std::vector<Ipv6Address>> nodeAddresses(numNodes);
    
    // First, create the small-world network topology with point-to-point connections
    // This will populate nodeAddresses with the correct addresses for each node
    CreateSmallWorldNetworkP2P(nodes, pointToPoint, ipv6, nodeAddresses, 
                               numNodes, numPeers, rewireProbability);
    
    // Now create gossip and miner applications with the correct addresses
    std::vector<Ptr<TcpGossipApp>> gossipApps;
    std::vector<Ptr<MinerApp>> minerApps;
    
    for (uint32_t i = 0; i < numNodes; i++) {
        // Create gossip app with the correct primary address (first interface address)
        Ipv6Address primaryAddress = nodeAddresses[i].empty() ? 
                                   Ipv6Address("::1") : nodeAddresses[i][0];
        
        Ptr<TcpGossipApp> app = CreateObject<TcpGossipApp>(primaryAddress);
        nodes.Get(i)->AddApplication(app);
        app->SetStartTime(Seconds(1.0));
        app->SetStopTime(Seconds(simulationTime));
        gossipApps.push_back(app);
        
        // Create and install the miner app with a delay
        Ptr<MinerApp> minerApp = CreateObject<MinerApp>();
        nodes.Get(i)->AddApplication(minerApp);
        minerApp->SetStartTime(Seconds(1.0));
        minerApp->SetStopTime(Seconds(simulationTime));
        minerApp->SetGossipApp(app);
        minerApp->SetSimulationStopTime(simulationTime);
        minerApps.push_back(minerApp);
    }
    
    // Add neighbors to gossip applications
    AddNeighborsToGossipApps(gossipApps, nodeAddresses, numNodes);

    
    for (uint32_t i = 0; i < numNodes; i++) {
        uint32_t senderIndex = rand() % numNodes;
        gossipApps[senderIndex]->SetSender();
    }
    
   // Create the network monitor
    NetworkMonitor monitor(gossipApps, minerApps, 30.0);

    // Schedule when to start and stop monitoring
    Simulator::Schedule(Seconds(5.0), &NetworkMonitor::Start, &monitor);
    Simulator::Schedule(Seconds(simulationTime - 1.0), &NetworkMonitor::Stop, &monitor);

    // Schedule progress reporting
    Simulator::Schedule(Seconds(1.0), &PrintSimulationProgress, 1.0);

    // Configure when to stop the simulation
    // Simulator::Schedule(Seconds(10.0), &exportDAGVisualization, nodeId);
    
    Simulator::Stop(Seconds(simulationTime));

    // Run the simulation
    std::cout << "Running simulation for " << simulationTime << " seconds..." << std::endl;
    
    if (enableDAGExport) {
        double startTime = 0.0;
        double endTime = simulationTime;  // Your simulation end time
        ScheduleDAGExports(startTime, dagExportInterval, endTime);
        
        // Schedule final export at the end
        Simulator::Schedule(Seconds(endTime), &ExportFinalDAGState);
    }

    Simulator::Run(); 
    // Collect results
    std::cout << "\nSimulation completed. Results:" << std::endl;
    std::cout << "Total blocks mined: " << MinerApp::totalBlocksMined << std::endl;
    
    // Calculate block propagation statistics
    uint32_t totalReceivedBlocks = 0;
    uint32_t minReceivedBlocks = UINT32_MAX;
    uint32_t maxReceivedBlocks = 0;
    
    std::map<uint32_t, uint32_t> blockReceiptDistribution;
    
    for (uint32_t i = 0; i < numNodes; i++) {
        uint32_t receivedBlocks = gossipApps[i]->GetReceivedBlockCount();
        totalReceivedBlocks += receivedBlocks;
        minReceivedBlocks = std::min(minReceivedBlocks, receivedBlocks);
        maxReceivedBlocks = std::max(maxReceivedBlocks, receivedBlocks);
        
        blockReceiptDistribution[receivedBlocks]++;
    }
    
    double avgReceivedBlocks = static_cast<double>(totalReceivedBlocks) / numNodes;
    double propagationRatio = (avgReceivedBlocks / MinerApp::totalBlocksMined) * 100.0;
    
    std::cout << "Block propagation statistics:" << std::endl;
    std::cout << "  Average blocks received per node: " << std::setprecision(2) << avgReceivedBlocks 
              << " (" << std::setprecision(2) << propagationRatio << "% of total blocks)" << std::endl;
    std::cout << "  Min blocks received: " << minReceivedBlocks << std::endl;
    std::cout << "  Max blocks received: " << maxReceivedBlocks << std::endl;
    
    // Print distribution of block receipt
    std::cout << "\nBlock receipt distribution:" << std::endl;
    for (auto& pair : blockReceiptDistribution) {
        double percentage = static_cast<double>(pair.second) / numNodes * 100.0;
        std::cout << "  " << pair.first << " blocks: " << pair.second << " nodes (" 
                  << std::setprecision(2) << percentage << "%)" << std::endl;
    }
    
        
    // Print blocks mined by each node in ascending order by node ID
    std::cout << "\nBlocks mined by each node:" << std::endl;
    std::vector<std::pair<uint32_t, uint32_t>> minerStats;
    for (auto& pair : MinerApp::perNodeMinedBlocks) {
        minerStats.push_back(pair);
    }

    // Sort by node ID in ascending order
    std::sort(minerStats.begin(), minerStats.end(), 
            [](const auto& a, const auto& b) { return a.first < b.first; });

    // Print all nodes
    for (const auto& pair : minerStats) {
        std::cout << "  Node " << pair.first << ": " << pair.second << " blocks" << std::endl;
    }
    
    // Calculate network connectivity statistics
    uint32_t totalConnections = 0;
    uint32_t minConnections = UINT32_MAX;
    uint32_t maxConnections = 0;
    
    for (uint32_t i = 0; i < numNodes; i++) {
        uint32_t connectionCount = gossipApps[i]->GetConnectedNeighborCount();
        totalConnections += connectionCount;
        minConnections = std::min(minConnections, connectionCount);
        maxConnections = std::max(maxConnections, connectionCount);
    }
    
    double avgConnections = static_cast<double>(totalConnections) / numNodes;
    
    std::cout << "\nNetwork connectivity statistics:" << std::endl;
    std::cout << "  Average active connections per node: " << std::setprecision(2) << avgConnections << std::endl;
    std::cout << "  Min active connections: " << minConnections << std::endl;
    std::cout << "  Max active connections: " << maxConnections << std::endl;
    

NS_LOG_INFO("\n\n");
NS_LOG_INFO("################################################################################");
NS_LOG_INFO("                           SIMULATION COMPLETED");
NS_LOG_INFO("################################################################################");

// Print overall mining statistics
NS_LOG_INFO("Total blocks mined across all nodes: " << MinerApp::totalBlocksMined);
NS_LOG_INFO("Blocks per node:");
for (const auto& pair : MinerApp::perNodeMinedBlocks) {
    NS_LOG_INFO("  Node " << pair.first << ": " << pair.second << " blocks");
}

// Select nodes for detailed analysis using the new advanced selection
std::vector<uint32_t> selectedNodes = SelectRandomNodesAdvanced(numNodes, 10, true);

NS_LOG_INFO("\nSelected nodes for detailed blockchain analysis: ");
for (uint32_t nodeId : selectedNodes) {
    NS_LOG_INFO("  Node " << nodeId);
}

NS_LOG_INFO("\n");

// Convert NodeContainer to vector for easier access
std::vector<Ptr<Node>> nodeVector;
for (uint32_t i = 0; i < nodes.GetN(); ++i) {
    nodeVector.push_back(nodes.Get(i));
}

// **UPDATED ANALYSIS CALLS**

// 1. Network-wide consensus analysis (replaced the old AnalyzeNetworkConsensus)
AnalyzeNetworkConsensus(nodeVector, numNodes);

// 2. Detailed analysis for selected nodes (new advanced function)
PrintDetailedBlockchainAnalysis(nodeVector, selectedNodes);

// 3. Compare states between selected nodes (new comparison function)
CompareDAGBlockchainStates(nodeVector, selectedNodes);

// Optional: If you want to analyze ALL nodes (use with caution for large networks)
/*
NS_LOG_INFO("Analyzing ALL nodes:");
std::vector<uint32_t> allNodes;
for (uint32_t i = 0; i < numNodes; ++i) {
    allNodes.push_back(i);
}
PrintDetailedBlockchainAnalysis(nodeVector, allNodes);
CompareDAGBlockchainStates(nodeVector, allNodes);
*/
    
    if (enableDAGExport) {
        std::cout << "\n=== DAG EXPORT SUMMARY ===\n";
        std::cout << "DAG files exported to: " << outputDir << "\n";
        std::cout << "To visualize, run: ./" << outputDir << "visualize_all.sh\n";
        std::cout << "Or manually: dot -Tpng filename.dot -o filename.png\n";
        std::cout << "==========================\n\n";
    }



    // End timing
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Print timing results
    std::cout << "\n=== TIMING RESULTS ===" << std::endl;
    std::cout << "Simulation time configured: " << simulationTime << " seconds" << std::endl;
    std::cout << "Actual runtime: " << duration.count() << " milliseconds" << std::endl;
    std::cout << "Actual runtime: " << duration.count() / 1000.0 << " seconds" << std::endl;
    std::cout << "Simulation speed ratio: " << (simulationTime / (duration.count() / 1000.0)) << "x" << std::endl;
    std::cout << "=====================" << std::endl;

Simulator::Destroy();
    
    return 0;
}



