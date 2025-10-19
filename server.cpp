#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <algorithm>
#include <random>
#include <map>      // For hand evaluator
#include <set>      // For hand evaluator
#include <sstream>
#include <netinet/in.h>
#include <unistd.h>
#include <queue>
#include <chrono>
#include <limits> // For std::numeric_limits

#define PORT 5555
#define MAX_PLAYERS 4
#define STARTING_CHIPS 1000

// Thread-safe message queue for all client input
struct Message {
    int socket;
    std::string data;
};
std::queue<Message> g_inbound_messages;
std::mutex g_inbound_mutex;
std::mutex g_io_mutex; // For protecting std::cout
std::mutex g_players_mutex; // For protecting players list

// ===== Structures =====
struct Card {
    std::string rank, suit;
    
    // This function now TRANSLATES the suit for network transmission
    std::string toString() const {
        std::string asciiSuit = suit;
        if (suit == "♥") asciiSuit = "H";
        else if (suit == "♦") asciiSuit = "D";
        else if (suit == "♣") asciiSuit = "C";
        else if (suit == "♠") asciiSuit = "S";
        return rank + asciiSuit;
    }
};

struct Player {
    std::string name;
    int chips;
    bool folded;
    bool allIn;
    std::vector<Card> hand;
    int socket;
    bool isAI;
    int currentBet;
    bool isConnected;
    
    Player() : chips(STARTING_CHIPS), folded(false), allIn(false), socket(-1), 
               isAI(false), currentBet(0), isConnected(true) {}
};

// ===== NEW: Hand Result Struct =====
// The rank is now a 'long long' to store complex scores
struct HandResult {
    long long rank; // A score to compare (higher is better)
    std::string name; // "a Pair of Kings" or "High Card Ace"
};


// ===== Global Variables =====
std::vector<Player> players;
std::vector<Card> deck;
std::vector<Card> communityCards;
int pot = 0;
int currentBet = 0;

// ===== Utility Functions =====
Player* getPlayerBySocket(int socket) {
    for (auto &p : players) {
        if (p.socket == socket) return &p;
    }
    return nullptr;
}

// Broadcasts without locking the player mutex.
// ONLY call this when you are already holding the g_players_mutex!
void broadcast_unsafe(const std::string &msg) {
    std::string fullMsg = msg + "\n";
    for(auto &p : players) {
        if(!p.isAI && p.socket != -1 && p.isConnected) {
            send(p.socket, fullMsg.c_str(), fullMsg.size(), 0);
        }
    }
}

void broadcast(const std::string &msg) {
    std::string fullMsg = msg + "\n";
    std::lock_guard<std::mutex> lock(g_players_mutex);
    broadcast_unsafe(msg); // Call the unsafe version
}

void sendToPlayer(Player &p, const std::string &msg) {
    if(!p.isAI && p.socket != -1 && p.isConnected) {
        std::string fullMsg = msg + "\n";
        send(p.socket, fullMsg.c_str(), fullMsg.size(), 0);
    }
}

void broadcastChat(const std::string &playerName, const std::string &message) {
    std::string msg = "CHAT:" + playerName + ":" + message;
    broadcast(msg);
    std::lock_guard<std::mutex> lock(g_io_mutex);
    std::cout << "[CHAT] " << playerName << ": " << message << std::endl;
}

// ===== Deck & Cards =====
void createDeck(){
    deck.clear();
    // Server uses Unicode suits internally
    std::vector<std::string> suits{"♥","♦","♣","♠"};
    std::vector<std::string> ranks{"2","3","4","5","6","7","8","9","10","J","Q","K","A"};
    for(auto &s:suits) for(auto &r:ranks) deck.push_back(Card{r,s});
}

void shuffleDeck(){ std::shuffle(deck.begin(),deck.end(),std::mt19937(std::random_device{}())); }
Card drawCard(){ Card c=deck.back(); deck.pop_back(); return c; }

// This function displays cards on the SERVER terminal
std::string displayCards(const std::vector<Card> &cards){
    std::stringstream ss;
    for(int line=0;line<5;line++){
        for(auto &c:cards){
            if(line==0) ss<<"┌─────┐ "; 
            else if(line==1) ss<<"│"<<c.rank<<(c.rank.size()==1?"    │ ":"   │ ");
            else if(line==2) ss<<"│  "<<c.suit<<"  │ "; // Directly prints Unicode suit
            else if(line==3) ss<<"│"<<(c.rank.size()==1?"    ":"   ")<<c.rank<<"│ ";
            else ss<<"└─────┘ ";
        }
        ss<<"\n";
    }
    return ss.str();
}

// ===== Showdown Helpers (Full Evaluator) =====

// Gets the numeric value of a card
int getCardValue(const std::string& rank) {
    if (rank == "A") return 14;
    if (rank == "K") return 13;
    if (rank == "Q") return 12;
    if (rank == "J") return 11;
    return std::stoi(rank);
}

// Helper to get the name of a card value
std::string getRankName(int val) {
    if (val == 14) return "Ace";
    if (val == 13) return "King";
    if (val == 12) return "Queen";
    if (val == 11) return "Jack";
    if (val == 10) return "10";
    if (val <= 9) return std::to_string(val);
    return "?";
}

// Helper to build a "kicker" score (e.g., A, K, 7, 5, 2)
// This is crucial for comparing two hands of the same type
long long getKickerScore(const std::vector<int>& kickers) {
    long long score = 0;
    long long multiplier = 100000000; // 10^8
    for(int kicker : kickers) {
        score += kicker * multiplier;
        multiplier /= 100;
    }
    return score;
}

// This is the new 5-card evaluator
HandResult evaluate5CardHand(std::vector<Card>& hand) {
    if (hand.size() != 5) return {0, "Invalid Hand"};

    // Sort hand by rank, high to low
    std::sort(hand.begin(), hand.end(), [](const Card& a, const Card& b) {
        return getCardValue(a.rank) > getCardValue(b.rank);
    });

    // Get all 5 ranks and check for flush
    std::vector<int> ranks;
    std::set<std::string> suits;
    for(const auto& c : hand) {
        ranks.push_back(getCardValue(c.rank));
        suits.insert(c.suit);
    }
    bool isFlush = (suits.size() == 1);

    // Check for straight
    bool isStraight = true;
    for(int i = 0; i < 4; ++i) {
        if (ranks[i] != ranks[i+1] + 1) isStraight = false;
    }
    // Check for A-2-3-4-5 straight (ranks will be 14, 5, 4, 3, 2)
    if (!isStraight && ranks[0] == 14 && ranks[1] == 5 && ranks[2] == 4 && ranks[3] == 3 && ranks[4] == 2) {
        isStraight = true;
        ranks = {5, 4, 3, 2, 1}; // Treat Ace as 1 for scoring this hand
    }

    // --- Check Hand Types, High to Low ---
    // Rank = (Hand Type * 10^12) + Kicker Score
    
    // 9. Straight Flush
    if (isStraight && isFlush) {
        if (ranks[0] == 14) return {9000000000000, "a Royal Flush"}; // 14 is Ace
        return {8000000000000 + ranks[0], "a Straight Flush (" + getRankName(ranks[0]) + " high)"};
    }

    // Count rank frequencies (e.g., 3 Kings, 2 Aces)
    std::map<int, int> rankCounts;
    for(int r : ranks) rankCounts[r]++;
    
    int fourOfAKindRank = 0;
    int threeOfAKindRank = 0;
    std::vector<int> pairs;
    std::vector<int> kickers;

    for (auto const& [rank, count] : rankCounts) {
        if (count == 4) fourOfAKindRank = rank;
        else if (count == 3) threeOfAKindRank = rank;
        else if (count == 2) pairs.push_back(rank);
        else kickers.push_back(rank);
    }
    std::sort(pairs.rbegin(), pairs.rend());
    std::sort(kickers.rbegin(), kickers.rend());

    // 8. Four of a Kind
    if (fourOfAKindRank > 0) {
        long long score = 7000000000000 + (fourOfAKindRank * 100) + kickers[0];
        return {score, "Four of a Kind (" + getRankName(fourOfAKindRank) + "s)"};
    }
    
    // 7. Full House
    if (threeOfAKindRank > 0 && pairs.size() > 0) {
        long long score = 6000000000000 + (threeOfAKindRank * 100) + pairs[0];
        return {score, "a Full House (" + getRankName(threeOfAKindRank) + "s full of " + getRankName(pairs[0]) + "s)"};
    }
    
    // 6. Flush
    if (isFlush) {
        return {5000000000000 + getKickerScore(ranks), "a Flush (" + getRankName(ranks[0]) + " high)"};
    }
    
    // 5. Straight
    if (isStraight) {
        return {4000000000000 + ranks[0], "a Straight (" + getRankName(ranks[0]) + " high)"};
    }
    
    // 4. Three of a Kind
    if (threeOfAKindRank > 0) {
        long long score = 3000000000000 + (threeOfAKindRank * 10000) + (kickers[0] * 100) + kickers[1];
        return {score, "Three of a Kind (" + getRankName(threeOfAKindRank) + "s)"};
    }

    // 3. Two Pair
    if (pairs.size() >= 2) {
        long long score = 2000000000000 + (pairs[0] * 10000) + (pairs[1] * 100) + kickers[0];
        return {score, "Two Pair (" + getRankName(pairs[0]) + "s and " + getRankName(pairs[1]) + "s)"};
    }
    
    // 2. One Pair
    if (pairs.size() == 1) {
        long long score = 1000000000000 + (pairs[0] * 1000000) + (kickers[0] * 10000) + (kickers[1] * 100) + kickers[2];
        return {score, "a Pair of " + getRankName(pairs[0]) + "s"};
    }

    // 1. High Card
    return {getKickerScore(ranks), "High Card " + getRankName(ranks[0])};
}

// This is the new 7-card evaluator manager
// It replaces the old 'getPlayerHandDescription'
HandResult getFullPlayerHand(Player& p) {
    std::vector<Card> all7Cards = p.hand; // Start with 2 hole cards
    all7Cards.insert(all7Cards.end(), communityCards.begin(), communityCards.end());

    HandResult bestHand = {0, "Nothing"};
    if (all7Cards.size() < 5) {
        // This can happen if hand ends pre-flop
        // Just score their hole cards (placeholder)
        if (p.hand.empty()) return {0, "Nothing"};
        int val1 = getCardValue(p.hand[0].rank);
        int val2 = getCardValue(p.hand[1].rank);
        if (val1 == val2) return {1000000000000 + val1, "a Pair of " + getRankName(val1) + "s"};
        return {std::max(val1, val2), "High Card " + getRankName(std::max(val1, val2))};
    }
    if (all7Cards.size() < 7) {
        // Hand ended on Flop or Turn, just evaluate the 5 or 6 cards
        return evaluate5CardHand(all7Cards); // This isn't perfect, but good enough for now
    }


    // Generate all combinations of 7 choose 5 (21 combinations)
    std::vector<bool> v(7);
    std::fill(v.begin() + 5, v.end(), false);
    std::fill(v.begin(), v.begin() + 5, true);

    do {
        std::vector<Card> current5CardHand;
        for (int i = 0; i < 7; ++i) {
            if (v[i]) {
                current5CardHand.push_back(all7Cards[i]);
            }
        }
        
        HandResult currentResult = evaluate5CardHand(current5CardHand);
        if (currentResult.rank > bestHand.rank) {
            bestHand = currentResult;
        }
    } while (std::next_permutation(v.begin(), v.end()));

    return bestHand;
}


// ===== AI Logic =====
int evaluateHand(const std::vector<Card> &hand, const std::vector<Card> &community);

std::string AIAction(Player &ai, int roundNumber){ // Added roundNumber
    int callAmt = currentBet - ai.currentBet;
    if (callAmt == 0) return "CHECK";
    if (callAmt > ai.chips / 2) return "FOLD";
    
    // Placeholder AI: Use the new evaluator
    HandResult aiHand = getFullPlayerHand(ai);
    
    // Simple logic:
    // 10^12 = 1 Pair, 2*10^12 = 2 Pair, 3*10^12 = 3 of a Kind...
    if (aiHand.rank > 2000000000000) { // Two Pair or better
        if (roundNumber == 0) return "RAISE 100"; // Pre-flop
        else return "RAISE 200"; // Post-flop
    } else if (aiHand.rank > 1000000000000) { // One Pair
        return "CALL";
    } else { // High Card
        if (roundNumber == 0 && callAmt < 50) return "CALL"; // Call small pre-flop
        return "FOLD";
    }
}
int evaluateHand(const std::vector<Card> &hand, const std::vector<Card> &community){
    // This function is no longer used by the "smarter" AI
    return 0;
}

// ===== Table Display =====
void showTable(){
    std::stringstream ss;
    ss<<"\nPot: "<<pot<<"\nPlayers:\n";
    {
        std::lock_guard<std::mutex> lock(g_players_mutex);
        for(auto &p:players){
            ss<<p.name<<"\t"<<p.chips<<" chips\t";
            if (!p.isConnected) ss << "DISCONNECTED";
            else if (p.folded) ss << "FOLDED";
            else ss << "ACTIVE";
            if (p.allIn) ss << " (ALL-IN)";
            if (p.currentBet > 0) ss << " (Bet: " << p.currentBet << ")";
            ss << "\n";
        }
    }
    
    broadcast(ss.str());

    if(communityCards.size() > 0) {
        std::stringstream ss_cards;
        ss_cards << "CARDS";
        for(auto& c : communityCards) {
            ss_cards << " " << c.toString();
        }
        broadcast(ss_cards.str());
    }
    
    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << ss.str();
        if(communityCards.size()>0){
            std::cout << "Community Cards:\n" << displayCards(communityCards);
        }
    }
}

// ===== NEW: Reset Function =====
void resetForNextHand() {
    std::lock_guard<std::mutex> lock(g_players_mutex);
    
    pot = 0;
    currentBet = 0;
    communityCards.clear();
    
    for (auto it = players.begin(); it != players.end(); /* no increment */) {
        if (!it->isConnected || it->chips <= 0) {
            if (!it->isAI) {
                std::cout << it->name << " has been removed from the game." << std::endl;
            }
            it = players.erase(it);
        } else {
            it->hand.clear();
            it->folded = false;
            it->allIn = false;
            it->currentBet = 0;
            ++it;
        }
    }
    
    createDeck();
    shuffleDeck();
}

// ===== Handle Incoming Messages =====
void handleIncomingMessage(int socket, const std::string& msg_data) {
    Player* p = getPlayerBySocket(socket);
    if (!p) return;

    if (msg_data == "DISCONNECTED") {
        p->isConnected = false;
        p->folded = true;
        broadcast(p->name + " has disconnected.");
    } else if (msg_data.find("CHAT:") == 0) {
        broadcastChat(p->name, msg_data.substr(5));
    }
}

// ===== Player Input =====
std::string getPlayerInput(Player &p) {
    sendToPlayer(p, "YOUR_MOVE");
    while (true) {
        Message msg;
        bool foundMessage = false;
        {
            std::lock_guard<std::mutex> lock(g_inbound_mutex);
            if (!g_inbound_messages.empty()) {
                msg = g_inbound_messages.front();
                g_inbound_messages.pop();
                foundMessage = true;
            }
        } 

        if (foundMessage) {
            if (msg.socket == p.socket) {
                if (msg.data.find("CHAT:") == 0) {
                    handleIncomingMessage(msg.socket, msg.data);
                    continue; 
                }
                else if (msg.data == "DISCONNECTED") {
                    handleIncomingMessage(msg.socket, msg.data);
                    return "FOLD";
                }
                return msg.data;
            } else {
                handleIncomingMessage(msg.socket, msg.data);
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ===== Betting Round =====
void bettingRound(int roundNumber) { // Modified to accept roundNumber
    int raisesThisRound = 0;
    currentBet = 0;
    for (auto& p : players) {
        p.currentBet = 0;
    }

    int turnIndex = 0;
    
    while(true) {
        int activePlayers = 0;
        for(auto &p : players) {
            if(!p.folded && !p.allIn && p.isConnected) activePlayers++;
        }
        if(activePlayers <= 1) break; // Hand is over

        Player &p = players[turnIndex % players.size()];

        if(!p.folded && !p.allIn && p.isConnected) {
            showTable();
            std::string action;

            if(p.isAI) {
                action = AIAction(p, roundNumber); // Pass roundNumber to AI
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << p.name << " chooses " << action << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            } else {
                action = getPlayerInput(p);
            }

            int callAmt = currentBet - p.currentBet;

            if(action.find("FOLD") != std::string::npos) {
                p.folded = true;
                broadcast(p.name + " folds.");
            }
            else if(action.find("CHECK") != std::string::npos) {
                if (callAmt == 0) {
                    broadcast(p.name + " checks.");
                } else {
                    broadcast(p.name + " tried to check. Folding.");
                    p.folded = true;
                }
            }
            else if(action.find("CALL") != std::string::npos) {
                if (callAmt == 0) {
                    broadcast(p.name + " checks."); 
                } else {
                    if (callAmt >= p.chips) {
                        callAmt = p.chips;
                        p.allIn = true;
                        broadcast(p.name + " calls and is ALL-IN!");
                    } else {
                        broadcast(p.name + " calls " + std::to_string(callAmt) + ".");
                    }
                    p.chips -= callAmt;
                    pot += callAmt;
                    p.currentBet += callAmt;
                }
            }
            else if(action.find("RAISE") != std::string::npos) {
                int raiseAmt = 0;
                try {
                    std::string amtStr = action.substr(action.find(" ") + 1);
                    raiseAmt = std::stoi(amtStr);
                } catch (...) {
                    raiseAmt = 50;
                }
                
                int totalBet = currentBet + raiseAmt;
                int amountToPutIn = totalBet - p.currentBet;

                if (amountToPutIn >= p.chips) {
                    amountToPutIn = p.chips;
                    totalBet = p.currentBet + amountToPutIn;
                    p.allIn = true;
                    broadcast(p.name + " raises and is ALL-IN!");
                } else {
                    broadcast(p.name + " raises " + std::to_string(raiseAmt) + ".");
                }

                p.chips -= amountToPutIn;
                pot += amountToPutIn;
                p.currentBet = totalBet;
                currentBet = totalBet;
                raisesThisRound++;
            }
            else {
                broadcast(p.name + " sent an unknown command. Folding.");
                p.folded = true;
            }
        }

        turnIndex++;

        bool roundFinished = true;
        int lastBet = -1;
        int activeInRound = 0;
        for(auto &pl : players) {
            if(pl.folded || !pl.isConnected) continue;
            
            if (!pl.allIn) {
                if (lastBet == -1) {
                    lastBet = pl.currentBet;
                }
                if (pl.currentBet != lastBet) {
                    roundFinished = false;
                }
                activeInRound++;
            }
        }
        
        if (activeInRound > 0 && roundFinished && turnIndex >= players.size()) {
            break;
        }
        
        if (activePlayers <= 1) {
            break;
        }
    }

    for(auto &p:players) p.currentBet = 0;
}

// ===== Client Handler =====
void clientHandler(int clientSocket) {
    char buffer[1024];
    std::string networkBuffer = "";
    
    try {
        while(true) {
            int valread = read(clientSocket, buffer, 1023);
            if(valread <= 0) {
                std::lock_guard<std::mutex> lock(g_inbound_mutex);
                g_inbound_messages.push({clientSocket, "DISCONNECTED"});
                break;
            }
            buffer[valread] = '\0';
            networkBuffer += buffer;

            size_t pos;
            while ((pos = networkBuffer.find('\n')) != std::string::npos) {
                std::string message = networkBuffer.substr(0, pos);
                networkBuffer.erase(0, pos + 1);
                message.erase(std::remove(message.begin(), message.end(), '\r'), message.end());

                std::lock_guard<std::mutex> lock(g_inbound_mutex);
                g_inbound_messages.push({clientSocket, message});
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cerr << "Exception in clientHandler: " << e.what() << std::endl;
    }
    close(clientSocket);
}

// ===== FIX: NEW Function to End Hand Early =====
bool checkIfHandOver() {
    int activePlayers = 0;
    Player* winner = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(g_players_mutex);
        for (auto& p : players) {
            if (!p.folded && p.isConnected) {
                activePlayers++;
                winner = &p;
            }
        }
    }

    if (activePlayers <= 1 && winner != nullptr) {
        std::string winMsg = winner->name + " wins the pot of " + std::to_string(pot) + " chips as the last player standing!";
        broadcast(winMsg);
        {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            std::cout << winMsg << std::endl;
        }
        winner->chips += pot;
        return true; // Hand is over
    }
    return false; // Hand continues
}

// ===== Main =====
int main(){
    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << "Is AI a player? (y/n): ";
        char choice; std::cin >> choice; std::cin.ignore();
        if(choice == 'y' || choice == 'Y') {
            Player ai; ai.name = "AI_Bot"; ai.isAI = true;
            players.push_back(ai);
            std::cout << "AI player 'AI_Bot' has joined.\n";
        }
    }

    int server_fd; struct sockaddr_in address; int opt=1; socklen_t addrlen=sizeof(address);
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    address.sin_family = AF_INET; address.sin_addr.s_addr = INADDR_ANY; address.sin_port = htons(PORT);
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, MAX_PLAYERS);
    
    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << "Server started on port " << PORT << ". Waiting for players...\n";
    }

    // --- Accept Thread ---
    std::thread([&](){
        while(true) { 
            int new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
            if (new_socket < 0) continue;
            
            char buffer[1024] = {0};
            int valread = read(new_socket, buffer, 1024);
            if (valread <= 0) {
                close(new_socket);
                continue;
            }
            std::string playerName(buffer, valread);
            playerName.erase(std::remove(playerName.begin(), playerName.end(), '\n'), playerName.end());
            playerName.erase(std::remove(playerName.begin(), playerName.end(), '\r'), playerName.end());

            {
                std::lock_guard<std::mutex> lock(g_players_mutex);
                if (players.size() >= MAX_PLAYERS) {
                    send(new_socket, "SERVER_FULL\n", 12, 0);
                    close(new_socket);
                    continue;
                }
                Player p;
                p.name = playerName;
                p.socket = new_socket;
                players.push_back(p);
                send(new_socket, ("WELCOME " + playerName + "\n").c_str(), playerName.size() + 9, 0);
                std::cout << playerName << " has connected.\n";
            }
            std::thread(clientHandler, new_socket).detach();
        }
    }).detach();

    // --- Lobby Loop ---
    std::string command;
    while(true) {
        {
            std::lock_guard<std::mutex> lock(g_players_mutex);
            std::cout << "\nConnected players (" << players.size() << "/" << MAX_PLAYERS << "): ";
            for(auto &p : players) std::cout << p.name << " ";
            std::cout << std::endl;
        }
        std::cout << "Type 'start' to begin the game (needs >= 2 players): ";
        std::getline(std::cin, command);
        
        if(command == "start") {
            if (players.size() >= 2) break;
            std::cout << "Need at least 2 players to start.\n";
        }
    }

    // ******************************************************
    // *** Main Game Loop                                 ***
    // ******************************************************
    while(true) {
        
        resetForNextHand();

        if (players.size() < 2) {
            std::cout << "Not enough players to continue. Shutting down." << std::endl;
            broadcast("Not enough players. Server shutting down.");
            break;
        }

        broadcast("GAME_STARTING");

        // Deal hole cards
        for(auto &p : players) {
            if(p.hand.empty()) {
                p.hand.push_back(drawCard());
                p.hand.push_back(drawCard());
                if(!p.isAI) {
                    sendToPlayer(p, "HOLE " + p.hand[0].toString() + " " + p.hand[1].toString());
                } else {
                    std::lock_guard<std::mutex> lock(g_io_mutex);
                    std::cout << "AI_Bot hole cards:\n" << displayCards(p.hand);
                }
            }
        }

        // ===== Game Rounds (Pass round number) =====
        bettingRound(0); // Pre-flop
        if (checkIfHandOver()) continue; 
        
        // Flop
        for(int i=0;i<3;i++) communityCards.push_back(drawCard());
        showTable();
        bettingRound(1); // Bet *after* flop
        if (checkIfHandOver()) continue;
        
        // Turn
        communityCards.push_back(drawCard());
        showTable();
        bettingRound(2); // Bet *after* turn
        if (checkIfHandOver()) continue;
        
        // River
        communityCards.push_back(drawCard());
        showTable();
        bettingRound(3); // Bet *after* river
        if (checkIfHandOver()) continue;

        // ===== SHOWDOWN (CHANGED) =====
        broadcast("\n--- SHOWDOWN ---");
        {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            std::cout << "\n--- SHOWDOWN ---" << std::endl;
        }
        
        Player* winner = nullptr;
        HandResult bestHand = {0, "Nothing"}; // Use the new struct

        {
            std::lock_guard<std::mutex> lock(g_players_mutex);
            for (auto& p : players) {
                if (!p.folded && p.isConnected) {
                    // Create the string for the client (using ASCII)
                    std::string broadcastHandStr = p.name + "'s hand: " + p.hand[0].toString() + " " + p.hand[1].toString();
                    broadcast_unsafe(broadcastHandStr); 
                    
                    // Create the string for the server (using Unicode)
                    std::string coutHandStr = p.name + "'s hand: " + p.hand[0].rank + p.hand[0].suit + " " + p.hand[1].rank + p.hand[1].suit;
                    {
                        std::lock_guard<std::mutex> io_lock(g_io_mutex);
                        std::cout << coutHandStr << std::endl; // Print the Unicode one
                    }
                    
                    // THIS IS THE ONLY LINE THAT'S DIFFERENT
                    HandResult hand = getFullPlayerHand(p); // Call the new 7-card evaluator
                    
                    if (hand.rank > bestHand.rank) {
                        bestHand = hand;
                        winner = &p;
                    }
                }
            }
        }

        if (winner != nullptr) {
            // NEW: Announce *how* they won
            std::string winMsg = winner->name + " wins the pot of " + std::to_string(pot) + " chips with " + bestHand.name + "!";
            broadcast(winMsg);
            {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << winMsg << std::endl;
            }
            winner->chips += pot;
        } else {
            std::string noWinnerMsg = "No winner, pot is split (logic not implemented).";
            broadcast(noWinnerMsg);
            {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << noWinnerMsg << std::endl;
            }
        }

        // ===== "Another Round?" Y/N Pause =====
        std::string choice;
        {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            std::cout << "--- Hand Over ---" << std::endl;
            std::cout << "Another round? (y/n): ";
        }
        
        broadcast("HAND_OVER\nWaiting for server admin to start the next round...");
        
        // FIX: Just clear error flags. Do NOT ignore the buffer.
        std::cin.clear();
        
        std::getline(std::cin, choice);
        
        if (choice.empty() || (choice[0] != 'y' && choice[0] != 'Y')) {
            broadcast("The game is ending. Thanks for playing!");
            {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << "Shutting down." << std::endl;
            }
            break; 
        }
    
    } // End of while(true) game loop


    std::cout << "Game Over.\n";
    return 0;
}