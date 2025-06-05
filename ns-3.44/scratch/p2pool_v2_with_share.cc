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
                    m_connectionPriority[neighbor] = rand() % 100; // Random initial priority
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
                // Update last activity time
                m_lastActivity[neighbor] = Simulator::Now().GetSeconds();
                
                // Increase priority (lower number = higher priority)
                m_connectionPriority[neighbor] = m_connectionPriority[neighbor] / 2;
            }
            
            void UpdateActivityFromSocket(Ptr<Socket> socket) {
                // Find which neighbor this socket belongs to
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
            
            // Get neighbors ordered by priority
            std::vector<Ipv6Address> GetPriorityNeighbors() {
                std::vector<std::pair<Ipv6Address, uint32_t>> neighbors;
                
                for (const auto& pair : m_connectionPriority) {
                    neighbors.push_back(std::make_pair(pair.first, pair.second));
                }
                
                // Sort by priority (lower number = higher priority)
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
                // Check if we need to establish more connections
                uint32_t activeConnections = GetActiveConnectionCount();
                
                if (activeConnections < MAX_ACTIVE_CONNECTIONS && m_neighborSockets.size() > 0) {
                    // Get neighbors sorted by priority
                    std::vector<Ipv6Address> priorityNeighbors = GetPriorityNeighbors();
                    
                    // Try to establish connections to high-priority neighbors first
                    for (const auto& neighbor : priorityNeighbors) {
                        if (activeConnections >= MAX_ACTIVE_CONNECTIONS) break;
                        
                        if (!IsActive(neighbor)) {
                            // Schedule connection with a small delay
                            Simulator::Schedule(MilliSeconds(rand() % 100), 
                                              &TcpGossipApp::ConnectToNeighbor, 
                                              m_app, neighbor);
                            
                            activeConnections++;
                        }
                    }
                }
                
                // If we have too many connections, close the least important ones
                if (activeConnections > MAX_ACTIVE_CONNECTIONS) {
                    // Get neighbors in reverse priority order
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
            // Track received messages (blocks and other data)
            std::unordered_set<std::string> m_receivedMessages;
            std::unordered_set<std::string> m_forwardedMessages;
            
            // Specifically track blocks by hash
            std::unordered_set<std::string> m_receivedBlocks;
            uint32_t m_receivedBlockCount;
            
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
            
            void MarkReceived(const std::string& msg) {
                m_receivedMessages.insert(msg);
            }
            
            void MarkBlockReceived(const std::string& blockHash) {
                if (m_receivedBlocks.find(blockHash) == m_receivedBlocks.end()) {
                    m_receivedBlocks.insert(blockHash);
                    m_receivedBlockCount++;
                }
            }
            
            void MarkForwarded(const std::string& msg) {
                m_forwardedMessages.insert(msg);
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
        
        // For batched message forwarding
        EventId m_forwardEvent;
        std::vector<std::string> m_pendingMessages;
        static const uint32_t MAX_PENDING_MESSAGES = 20;
        static const Time FORWARD_INTERVAL;
    
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

        // Set reference to miner application
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
            
            // Only print neighbors for a small subset of nodes
            if (m_nodeId % 50 == 0) {
                PrintNeighbors();
            }
            
            // Create listening socket
            m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
            m_socket->Bind(Inet6SocketAddress(Ipv6Address::GetAny(), 8080));
            m_socket->Listen();
    
            m_socket->SetAcceptCallback(
                MakeCallback(&TcpGossipApp::AcceptConnection, this),
                MakeCallback(&TcpGossipApp::HandleAccept, this)
            );
            
            // Schedule connection establishment with delay based on node ID
            // This staggers connection attempts to prevent network flood
            Time delay = MilliSeconds(100 + (m_nodeId % 1000));
            Simulator::Schedule(delay, &TcpGossipApp::EstablishConnections, this);
            
            // Schedule periodic connection check with staggered timing
            m_connectionCheckEvent = Simulator::Schedule(
                Seconds(2.0 + (double)(m_nodeId % 100) / 100.0), 
                &TcpGossipApp::CheckConnections, this);
        }
        
        void StopApplication() override {
            // Cancel scheduled events
            if (m_connectionCheckEvent.IsRunning()) {
                Simulator::Cancel(m_connectionCheckEvent);
            }
            
            if (m_forwardEvent.IsRunning()) {
                Simulator::Cancel(m_forwardEvent);
            }
            
            // Close all connections
            m_connectionPool.CloseAllConnections();
            
            // Close listening socket
            if (m_socket) {
                m_socket->Close();
                m_socket = nullptr;
            }
        }
        
        void EstablishConnections() {
            // Let the connection pool manage connections
            m_connectionPool.ManageConnections();
            m_connectionsEstablished = true;
        }
        
        void ConnectToNeighbor(Ipv6Address neighborAddr) {
            // Create a new socket for this neighbor
            Ptr<Socket> socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
            
            socket->SetConnectCallback(
                MakeCallback(&TcpGossipApp::ConnectionSucceeded, this),
                MakeCallback(&TcpGossipApp::ConnectionFailed, this)
            );
            
            socket->SetRecvCallback(MakeCallback(&TcpGossipApp::ReceiveMessage, this));
            
            // Add this socket to our tracking
            m_connectionPool.SetSocket(neighborAddr, socket);
            m_connectionPool.SetSocketActive(neighborAddr, false); // Not active yet
            
            // Try to connect
            socket->Connect(Inet6SocketAddress(neighborAddr, 8080));
        }
        
        void CheckConnections() {
            // Manage connections periodically
            m_connectionPool.ManageConnections();
            
            // Schedule next check with a random variation to prevent synchronization
            double jitter = (double)(rand() % 500) / 1000.0; // 0-0.5s jitter
            m_connectionCheckEvent = Simulator::Schedule(
                Seconds(5.0 + jitter), 
                &TcpGossipApp::CheckConnections, this);
        }
        
        void SendHeartbeat(Ptr<Socket> socket) {
            // Send a compact heartbeat message
            std::string heartbeat = "h\n";
            Ptr<Packet> packet = Create<Packet>((uint8_t*)heartbeat.c_str(), heartbeat.size());
            socket->Send(packet);
        }
    
        bool AcceptConnection(Ptr<Socket> socket, const Address &from) {
            return true;  
        }
    
        void HandleAccept(Ptr<Socket> socket, const Address &from) {
            // Track this incoming socket
            m_connectionPool.AddIncomingSocket(socket);
            
            // Set up receive callback
            socket->SetRecvCallback(MakeCallback(&TcpGossipApp::ReceiveMessage, this));
        }
    
        void SendMessage(std::string msg) {
            if (m_messageManager.IsReceived(msg)) return;
    
            m_messageManager.MarkReceived(msg);
            AddToPendingMessages(msg);
        }

        // Enhanced method for sending blockchain messages (blocks)
        void SendBlockMessage(const std::string& serializedBlock) {
            // Parse the block to get its hash for duplicate checking
            std::string blockHash = ExtractBlockHash(serializedBlock);
            
            if (!blockHash.empty() && m_messageManager.IsBlockReceived(blockHash)) {
                return; // Already processed this block
            }
            
            // Mark as received and forward
            if (!blockHash.empty()) {
                m_messageManager.MarkBlockReceived(blockHash);
            }
            m_messageManager.MarkReceived(serializedBlock);
            AddToPendingMessages(serializedBlock);
        }
    
        void ReceiveMessage(Ptr<Socket> socket) {
            Address from;
            socket->GetPeerName(from);
            
            Ptr<Packet> packet = socket->Recv();
            if (!packet || packet->GetSize() == 0) {
                // Connection was closed or error
                m_connectionPool.RemoveIncomingSocket(socket);
                
                // Try to find which neighbor this was
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
            
            // Update activity on this connection
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
                // Update based on the socket directly
                m_connectionPool.UpdateActivityFromSocket(socket);
            }
        
            // Process the packet
            uint32_t size = packet->GetSize();
            std::vector<uint8_t> buffer(size);  
            packet->CopyData(buffer.data(), size);
            
            // Process potentially multiple messages in the buffer
            std::string data(buffer.begin(), buffer.end());
            std::istringstream stream(data);
            std::string line;
            
            // Parse each line as a separate message
            while (std::getline(stream, line)) {
                // Remove any carriage returns that might be present
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                
                // Skip empty lines
                if (line.empty()) continue;
                
                // Ignore heartbeat messages
                if (line == "h") {
                    continue;
                }
                
                // Process blockchain messages
                ProcessReceivedMessage(line);
            }
        }

        // Process different types of messages
        void ProcessReceivedMessage(const std::string& message);

        // Check if message is a blockchain message (serialized Share)
        bool IsBlockchainMessage(const std::string& message) {
            // Count pipe separators - blockchain messages have exactly 6 pipes
            size_t pipeCount = std::count(message.begin(), message.end(), '|');
            return pipeCount == 6;
        }

        // Extract block hash from serialized blockchain message
        std::string ExtractBlockHash(const std::string& serializedBlock) {
            if (!IsBlockchainMessage(serializedBlock)) {
                return "";
            }
            
            // Block hash is the first field before the first pipe
            size_t firstPipe = serializedBlock.find('|');
            if (firstPipe != std::string::npos) {
                return serializedBlock.substr(0, firstPipe);
            }
            
            return "";
        }
        
        void AddToPendingMessages(const std::string& msg) {
            m_pendingMessages.push_back(msg);
            
            // If this is the first pending message, schedule forwarding
            if (m_pendingMessages.size() == 1 && !m_forwardEvent.IsRunning()) {
                // Schedule with random delay to prevent network congestion
                Time delay = MilliSeconds(20 + (rand() % 200));
                m_forwardEvent = Simulator::Schedule(delay, &TcpGossipApp::ForwardPendingMessages, this);
            }
            // If we've reached the batch size, forward immediately
            else if (m_pendingMessages.size() >= MAX_PENDING_MESSAGES && !m_forwardEvent.IsRunning()) {
                m_forwardEvent = Simulator::Schedule(FORWARD_INTERVAL, &TcpGossipApp::ForwardPendingMessages, this);
            }
        }
        
        void ForwardPendingMessages() {
            if (m_pendingMessages.empty()) return;
            
            // Process all pending messages
            for (const auto& msg : m_pendingMessages) {
                if (!m_messageManager.IsForwarded(msg)) {
                    ForwardMessage(msg);
                    m_messageManager.MarkForwarded(msg);
                }
            }
            
            // Clear pending messages
            m_pendingMessages.clear();
        }
        
        void ForwardMessage(const std::string& msg) {
            // Add newline character to delimit messages
            std::string msgWithDelimiter = msg + "\n";
    
            // Get active neighbors in priority order
            std::vector<Ipv6Address> priorityNeighbors;
            for (const auto& neighbor : m_neighbors) {
                if (m_connectionPool.IsActive(neighbor)) {
                    priorityNeighbors.push_back(neighbor);
                }
            }
            
            // Random shuffle to distribute load
            std::random_shuffle(priorityNeighbors.begin(), priorityNeighbors.end());
            
            // Forward to a subset of neighbors to reduce network load
            // For blockchain messages, be more aggressive in forwarding
            uint32_t forwardCount;
            if (IsBlockchainMessage(msg)) {
                // Forward blocks to more neighbors for better propagation
                forwardCount = std::max(5u, (uint32_t)(priorityNeighbors.size() * 0.7));
            } else {
                // Regular messages use conservative forwarding
                forwardCount = std::max(3u, (uint32_t)(priorityNeighbors.size() * 0.5));
            }
            
            for (uint32_t i = 0; i < std::min(forwardCount, (uint32_t)priorityNeighbors.size()); i++) {
                Ipv6Address neighbor = priorityNeighbors[i];
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
            // on its next management cycle
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
    
    // Static member initialization
    const Time TcpGossipApp::FORWARD_INTERVAL = MilliSeconds(100);



struct Share {
    // Block identification
    std::string blockHash;           // Unique identifier for this block
    std::string parentHash;          // Hash of the parent block
    uint32_t blockNumber;            // Block height/number
    uint32_t nodeId;                 // ID of the node that mined this block
    
    // Timing information
    double timestamp;                // When this block was mined
    double receivedTime;             // When this node received the block (0 if self-mined)
    
    // Chain information
    uint32_t chainWeight;            // Total difficulty/weight of chain up to this block
    uint32_t difficulty;             // Difficulty of this specific block (can be 1 for simplicity)
    
    // Network propagation
    std::vector<uint32_t> propagationPath;  // Nodes that have seen this block
    
    // Constructor
    Share(const std::string& hash, const std::string& parent, uint32_t number, 
          uint32_t miner, double time, uint32_t diff = 1) 
        : blockHash(hash), parentHash(parent), blockNumber(number), 
          nodeId(miner), timestamp(time), receivedTime(0), 
          chainWeight(0), difficulty(diff) {
        propagationPath.push_back(miner);
    }
    
    // Copy constructor for received blocks
    Share(const Share& other, double receiveTime) 
        : blockHash(other.blockHash), parentHash(other.parentHash), 
          blockNumber(other.blockNumber), nodeId(other.nodeId), 
          timestamp(other.timestamp), receivedTime(receiveTime),
          chainWeight(other.chainWeight), difficulty(other.difficulty),
          propagationPath(other.propagationPath) {}
    
    // Calculate chain weight (simple sum of difficulties)
    void calculateChainWeight(const std::map<std::string, std::shared_ptr<Share>>& blockchain) {
        if (parentHash == "genesis" || parentHash.empty()) {
            chainWeight = difficulty;
        } else {
            auto parentIt = blockchain.find(parentHash);
            if (parentIt != blockchain.end()) {
                chainWeight = parentIt->second->chainWeight + difficulty;
            } else {
                chainWeight = difficulty; // Orphaned block
            }
        }
    }
    
    // Check if this block extends a given parent
    bool isChildOf(const std::string& hash) const {
        return parentHash == hash;
    }
    
    // Serialize for network transmission
    std::string serialize() const {
        std::stringstream ss;
        ss << blockHash << "|" << parentHash << "|" << blockNumber << "|" 
           << nodeId << "|" << timestamp << "|" << chainWeight << "|" << difficulty;
        return ss.str();
    }
    
    // Deserialize from network
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
        share->receivedTime = receiveTime;
        return share;
    }
};

enum class BlockRelation {
    PARENT,              // This block is our parent (we should build on it)
    CHILD,               // This block is our child (they built on us)
    SIBLING,             // Same parent, different blocks (potential uncle)
    UNCLE,               // Block from a competing chain at same height
    REORG_NEEDED,        // This block represents a heavier chain
    ORPHAN,              // Block with unknown parent
    DUPLICATE,           // Block we already have
    INVALID              // Invalid block (wrong parent relationship, etc.)
};

class BlockchainState {
private:
    std::map<std::string, std::shared_ptr<Share>> m_blockchain;
    std::string m_currentHead;
    std::map<uint32_t, std::vector<std::string>> m_blocksByHeight;  // Height -> list of block hashes
    uint32_t m_currentHeight;
    
public:
    BlockchainState() : m_currentHead("genesis"), m_currentHeight(0) {
        // Initialize genesis block
        auto genesis = std::make_shared<Share>("genesis", "", 0, 0, 0.0);
        genesis->chainWeight = 0;
        m_blockchain["genesis"] = genesis;
        m_blocksByHeight[0].push_back("genesis");
    }
    
    // Analyze relationship of incoming block
    BlockRelation analyzeBlock(const std::shared_ptr<Share>& newBlock) {
        // Check if we already have this block
        if (m_blockchain.find(newBlock->blockHash) != m_blockchain.end()) {
            return BlockRelation::DUPLICATE;
        }
        
        // Check if parent exists
        auto parentIt = m_blockchain.find(newBlock->parentHash);
        if (parentIt == m_blockchain.end()) {
            return BlockRelation::ORPHAN;
        }
        
        // Calculate chain weight for the new block
        auto blockCopy = std::make_shared<Share>(*newBlock, newBlock->receivedTime);
        blockCopy->calculateChainWeight(m_blockchain);
        
        // Check if this creates a heavier chain
        auto currentHead = m_blockchain[m_currentHead];
        if (blockCopy->chainWeight > currentHead->chainWeight) {
            return BlockRelation::REORG_NEEDED;
        }
        
        // Check relationships
        if (newBlock->parentHash == m_currentHead) {
            return BlockRelation::PARENT;  // Should be our new head
        }
        
        // Check if it's building on our current chain
        if (isOnMainChain(newBlock->parentHash)) {
            if (newBlock->blockNumber == m_currentHeight + 1) {
                return BlockRelation::PARENT;
            } else {
                return BlockRelation::UNCLE;  // Side chain
            }
        }
        
        // Check if same height as current head
        if (newBlock->blockNumber == m_currentHeight) {
            return BlockRelation::SIBLING;
        }
        
        return BlockRelation::UNCLE;
    }
    
    // Add block to blockchain and handle reorganization
    bool addBlock(const std::shared_ptr<Share>& newBlock) {
        BlockRelation relation = analyzeBlock(newBlock);
        
        switch (relation) {
            case BlockRelation::DUPLICATE:
                return false;  // Already have this block
                
            case BlockRelation::ORPHAN:
                // Store orphan blocks, might become valid later
                m_blockchain[newBlock->blockHash] = newBlock;
                return false;
                
            case BlockRelation::PARENT:
                // New head of the chain
                newBlock->calculateChainWeight(m_blockchain);
                m_blockchain[newBlock->blockHash] = newBlock;
                m_blocksByHeight[newBlock->blockNumber].push_back(newBlock->blockHash);
                m_currentHead = newBlock->blockHash;
                m_currentHeight = newBlock->blockNumber;
                return true;
                
            case BlockRelation::REORG_NEEDED:
                return performReorganization(newBlock);
                
            case BlockRelation::SIBLING:
            case BlockRelation::UNCLE:
                // Add to side chain
                newBlock->calculateChainWeight(m_blockchain);
                m_blockchain[newBlock->blockHash] = newBlock;
                m_blocksByHeight[newBlock->blockNumber].push_back(newBlock->blockHash);
                return false;  // Not new head, but stored
                
            default:
                return false;
        }
    }
    
    // Perform blockchain reorganization
    bool performReorganization(const std::shared_ptr<Share>& newBlock) {
        // Add the new block first
        newBlock->calculateChainWeight(m_blockchain);
        m_blockchain[newBlock->blockHash] = newBlock;
        m_blocksByHeight[newBlock->blockNumber].push_back(newBlock->blockHash);
        
        // Find the new heaviest chain
        std::string heaviestBlock = findHeaviestChain();
        
        if (heaviestBlock != m_currentHead) {
            std::string oldHead = m_currentHead;
            m_currentHead = heaviestBlock;
            m_currentHeight = m_blockchain[heaviestBlock]->blockNumber;
            
            NS_LOG_INFO("REORGANIZATION: Changed head from " << oldHead 
                       << " to " << heaviestBlock);
            return true;
        }
        
        return false;
    }
    
    // Find block with highest chain weight
    std::string findHeaviestChain() {
        std::string heaviest = m_currentHead;
        uint32_t maxWeight = m_blockchain[m_currentHead]->chainWeight;
        
        for (const auto& pair : m_blockchain) {
            if (pair.second->chainWeight > maxWeight) {
                maxWeight = pair.second->chainWeight;
                heaviest = pair.first;
            }
        }
        
        return heaviest;
    }
    
    // Check if block is on main chain
    bool isOnMainChain(const std::string& blockHash) {
        std::string current = m_currentHead;
        while (current != "genesis" && !current.empty()) {
            if (current == blockHash) {
                return true;
            }
            auto blockIt = m_blockchain.find(current);
            if (blockIt == m_blockchain.end()) break;
            current = blockIt->second->parentHash;
        }
        return blockHash == "genesis";
    }
    
    // Get current blockchain info
    std::string getCurrentHead() const { return m_currentHead; }
    uint32_t getCurrentHeight() const { return m_currentHeight; }
    
    // Get blocks at specific height (for uncle detection)
    std::vector<std::string> getBlocksAtHeight(uint32_t height) {
        auto it = m_blocksByHeight.find(height);
        return (it != m_blocksByHeight.end()) ? it->second : std::vector<std::string>();
    }
    
    // Print blockchain state for debugging
    void printState() const {
        NS_LOG_INFO("=== Blockchain State ===");
        NS_LOG_INFO("Current Head: " << m_currentHead);
        NS_LOG_INFO("Current Height: " << m_currentHeight);
        NS_LOG_INFO("Total Blocks: " << m_blockchain.size());
        
        // Print chain from head to genesis
        std::string current = m_currentHead;
        std::vector<std::string> mainChain;
        
        while (current != "genesis" && !current.empty()) {
            mainChain.push_back(current);
            auto blockIt = m_blockchain.find(current);
            if (blockIt == m_blockchain.end()) break;
            current = blockIt->second->parentHash;
        }
        mainChain.push_back("genesis");
        
        std::reverse(mainChain.begin(), mainChain.end());
        
        for (const auto& hash : mainChain) {
            auto block = m_blockchain.at(hash);
            NS_LOG_INFO("  " << hash << " (height: " << block->blockNumber 
                       << ", weight: " << block->chainWeight << ")");
        }
    }
};

// Updated MinerApp class to work with the integrated gossip system
class MinerApp : public Application {
    private:
        EventId m_miningEvent;
        uint32_t m_blockCounter = 0;
        bool m_running = false;
        Ptr<TcpGossipApp> m_gossipApp;
        double m_stopMiningTime = 0.0;
        
        // Blockchain state management
        std::unique_ptr<BlockchainState> m_blockchain;
        
        // Per-node random number generator for mining intervals
        Ptr<UniformRandomVariable> m_miningIntervalRNG;
        
    protected:
        // Fixed mining interval parameters
        static double s_minMiningInterval;
        static double s_maxMiningInterval;
        
    public:
        MinerApp() {
            // Initialize blockchain state
            m_blockchain = std::make_unique<BlockchainState>();
            
            // Initialize per-node random number generator
            m_miningIntervalRNG = CreateObject<UniformRandomVariable>();
            m_miningIntervalRNG->SetAttribute("Min", DoubleValue(s_minMiningInterval));
            m_miningIntervalRNG->SetAttribute("Max", DoubleValue(s_maxMiningInterval));
        }
        
        static uint32_t totalBlocksMined;
        static std::map<uint32_t, uint32_t> perNodeMinedBlocks;
        
        virtual void StartApplication() override {
            m_running = true;
            NS_LOG_INFO("MinerApp started on node " << GetNode()->GetId());
            
            // Start mining immediately with a random delay
            double initialDelay = m_miningIntervalRNG->GetValue();
            m_miningEvent = Simulator::Schedule(Seconds(initialDelay), &MinerApp::MineBlock, this);
            
            NS_LOG_INFO("Node " << GetNode()->GetId() << " will start mining in " << initialDelay << " seconds");
        }
        
        virtual void StopApplication() override {
            m_running = false;
            if (m_miningEvent.IsRunning()) {
                Simulator::Cancel(m_miningEvent);
            }
            
            // Print final blockchain state
            NS_LOG_INFO("Final blockchain state for node " << GetNode()->GetId() << ":");
            m_blockchain->printState();
        }
        
        void SetSimulationStopTime(double stopTime) {
            m_stopMiningTime = stopTime - 5.0;
        }
        
        void SetGossipApp(Ptr<TcpGossipApp> app) {
            m_gossipApp = app;
            // Set up bidirectional reference
            if (m_gossipApp) {
                m_gossipApp->SetMinerApp(this);
            }
        }
        
        uint32_t GetBlocksMined() const {
            return m_blockCounter;
        }
        
        // Called when a node receives a block from another node via gossip
        void OnBlockReceived(const std::string& blockData) {
            double currentTime = Simulator::Now().GetSeconds();
            
            // Deserialize the received block
            auto receivedBlock = Share::deserialize(blockData, currentTime);
            
            NS_LOG_INFO("Node " << GetNode()->GetId() << " received block " 
                       << receivedBlock->blockHash << " from node " << receivedBlock->nodeId);
            
            // Analyze the block relationship
            BlockRelation relation = m_blockchain->analyzeBlock(receivedBlock);
            
            // Log the relationship
            std::string relationStr = getRelationString(relation);
            NS_LOG_INFO("Node " << GetNode()->GetId() << " determined block relationship: " << relationStr);
            
            // Handle the block based on its relationship
            bool chainUpdated = false;
            switch (relation) {
                case BlockRelation::PARENT:
                    NS_LOG_INFO("Node " << GetNode()->GetId() << " accepting new head block");
                    chainUpdated = m_blockchain->addBlock(receivedBlock);
                    break;
                    
                case BlockRelation::REORG_NEEDED:
                    NS_LOG_INFO("Node " << GetNode()->GetId() << " performing reorganization");
                    chainUpdated = m_blockchain->addBlock(receivedBlock);
                    if (chainUpdated) {
                        // Restart mining on new head
                        restartMining();
                    }
                    break;
                    
                case BlockRelation::SIBLING:
                case BlockRelation::UNCLE:
                    NS_LOG_INFO("Node " << GetNode()->GetId() << " storing uncle/sibling block");
                    m_blockchain->addBlock(receivedBlock);
                    break;
                    
                case BlockRelation::ORPHAN:
                    NS_LOG_INFO("Node " << GetNode()->GetId() << " received orphan block - storing for later");
                    m_blockchain->addBlock(receivedBlock);
                    break;
                    
                case BlockRelation::DUPLICATE:
                    NS_LOG_DEBUG("Node " << GetNode()->GetId() << " received duplicate block");
                    break;
                    
                default:
                    NS_LOG_WARN("Node " << GetNode()->GetId() << " received invalid block");
                    break;
            }
            
            if (chainUpdated) {
                NS_LOG_INFO("Node " << GetNode()->GetId() << " chain updated. New head: " 
                           << m_blockchain->getCurrentHead() << " Height: " << m_blockchain->getCurrentHeight());
            }
        }
        
    private:
        void MineBlock() {
            if (!m_running || Simulator::Now().GetSeconds() >= m_stopMiningTime) {
                return;
            }
            
            // Create new block
            double currentTime = Simulator::Now().GetSeconds();
            std::string currentHead = m_blockchain->getCurrentHead();
            uint32_t newHeight = m_blockchain->getCurrentHeight() + 1;
            
            // Generate unique block hash
            std::string blockHash = generateBlockHash(newHeight, GetNode()->GetId(), currentTime);
            
            // Create the share/block
            auto newBlock = std::make_shared<Share>(blockHash, currentHead, newHeight, 
                                                  GetNode()->GetId(), currentTime);
            
            // Add to our blockchain
            bool added = m_blockchain->addBlock(newBlock);
            
            if (added) {
                m_blockCounter++;
                totalBlocksMined++;
                perNodeMinedBlocks[GetNode()->GetId()]++;
                
                NS_LOG_INFO("Node " << GetNode()->GetId() << " mined block " << blockHash 
                           << " (height: " << newHeight << ") at time " << currentTime);
                
                // Propagate the block to other nodes using the enhanced method
                if (m_gossipApp) {
                    std::string serializedBlock = newBlock->serialize();
                    m_gossipApp->SendBlockMessage(serializedBlock);
                }
            }
            
            // Schedule next mining
            ScheduleNextMining();
        }
        
        void ScheduleNextMining() {
            if (!m_running) return;
            
            double nextMiningInterval = m_miningIntervalRNG->GetValue();
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
            
            // Schedule new mining with short delay
            double restartDelay = 0.1; // 100ms delay before restarting
            m_miningEvent = Simulator::Schedule(Seconds(restartDelay), &MinerApp::MineBlock, this);
            
            NS_LOG_INFO("Node " << GetNode()->GetId() << " restarting mining after reorganization");
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
                default: return "UNKNOWN";
            }
        }
    };
    
    // Initialize static members
    uint32_t MinerApp::totalBlocksMined = 0;
    std::map<uint32_t, uint32_t> MinerApp::perNodeMinedBlocks;
    double MinerApp::s_minMiningInterval = 10.0;
    double MinerApp::s_maxMiningInterval = 30.0;


void TcpGossipApp::ProcessReceivedMessage(const std::string& message) {
            // Check if this is a blockchain message (contains pipe separators)
            if (IsBlockchainMessage(message)) {
                // Extract block hash for duplicate checking
                std::string blockHash = ExtractBlockHash(message);
                
                if (!blockHash.empty()) {
                    // Check if we've already processed this block
                    if (m_messageManager.IsBlockReceived(blockHash)) {
                        return; // Already processed
                    }
                    
                    // Mark as received
                    m_messageManager.MarkBlockReceived(blockHash);
                    
                    // Forward to miner app for processing
                    if (m_minerApp) {
                        // m_minerApp->OnBlockReceived(message);
                        
                    }
                }
            }
            
            // Process the message if not received before
            if (!m_messageManager.IsReceived(message)) {
                m_messageManager.MarkReceived(message);
                
                // Add to pending messages for forwarding
                AddToPendingMessages(message);
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


int main(int argc, char* argv[]) {
   
    // Seed the random number generator with current time
    srand(time(nullptr));
    CommandLine cmd;
    uint32_t numNodes = 201;
    uint32_t numPeers = 8;  // Changed to 8 connections per node
    double rewireProbability = 0.5;
    double simulationTime = 500.0;

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
    NodeContainer nodes;
    nodes.Create(numNodes);

    // Set up internet stack
    InternetStackHelper internet;
    internet.Install(nodes);

    // Create point-to-point helper
    PointToPointHelper pointToPoint;
    pointToPoint.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    pointToPoint.SetChannelAttribute("Delay", StringValue("10us"));

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
    Simulator::Stop(Seconds(simulationTime));

    // Run the simulation
    std::cout << "Running simulation for " << simulationTime << " seconds..." << std::endl;
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
    
    Simulator::Destroy();
    
    return 0;
}

