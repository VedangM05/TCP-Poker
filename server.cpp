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
#include <cstdio> // For snprintf in player table

#define PORT 5555
#define MAX_PLAYERS 4
#define STARTING_CHIPS 1000
#define MONTE_CARLO_SIMULATIONS 2000 // Higher = slower but smarter.
#define ANTE_AMOUNT 10

// Thread-safe message queue for all client input
struct Message {
    int socket;
    std::string data;
};
std::queue<Message> g_inbound_messages;
std::mutex g_inbound_mutex;
std::mutex g_io_mutex; // For protecting std::cout
std::mutex g_players_mutex; // For protecting players list

// ===== NEW: clearScreen Function =====
void clearScreen() {
    // Uses ANSI escape codes to clear the screen and move cursor to top-left
    std::cout << "\033[2J\033[1;1H";
}

// ===== Structures =====
struct Card {
    std::string rank, suit;

    std::string toString() const {
        std::string asciiSuit = suit;
        if (suit == "♥") asciiSuit = "H";
        else if (suit == "♦") asciiSuit = "D";
        else if (suit == "♣") asciiSuit = "C";
        else if (suit == "♠") asciiSuit = "S";
        return rank + asciiSuit;
    }

    bool operator==(const Card& other) const {
        return rank == other.rank && suit == other.suit;
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
    int handsPlayed = 0;
    int vpipActions = 0;
    int pfrActions = 0;

    Player() : chips(STARTING_CHIPS), folded(false), allIn(false), socket(-1),
               isAI(false), currentBet(0), isConnected(true) {}
};

struct HandResult {
    long long rank;
    std::string name;
};

// ===== Global Variables =====
std::vector<Player> players;
std::vector<Card> deck;
std::vector<Card> communityCards;
int pot = 0;
int currentBet = 0;
bool g_preFlopRaiseMade = false;

// ===== Utility Functions =====
Player* getPlayerBySocket(int socket) {
    for (auto &p : players) {
        if (p.socket == socket) {
            return &p;
        }
    }
    return nullptr;
}

Player* getHumanOpponent() {
    for (auto& p : players) {
        if (!p.isAI && !p.folded && p.isConnected) {
            return &p;
        }
    }
    return nullptr;
}

// --- UPDATED: Added send() error checking ---
void sendToPlayer(Player &p, const std::string &msg) {
    if (!p.isAI && p.socket != -1 && p.isConnected) {
        std::string fullMsg = msg + "\n";
        if (send(p.socket, fullMsg.c_str(), fullMsg.size(), 0) == -1) {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            std::cout << "[Network] Failed to send to " << p.name << std::endl;
        }
    }
}

// --- UPDATED: Added send() error checking ---
void broadcast_unsafe(const std::string &msg) {
    std::string fullMsg = msg + "\n";
    for (auto &p : players) {
        if (!p.isAI && p.socket != -1 && p.isConnected) {
            if (send(p.socket, fullMsg.c_str(), fullMsg.size(), 0) == -1) {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << "[Network] Failed to broadcast to " << p.name << std::endl;
            }
        }
    }
}

void broadcast(const std::string &msg) {
    std::lock_guard<std::mutex> lock(g_players_mutex);
    broadcast_unsafe(msg);
}

void broadcastChat(const std::string &playerName, const std::string &message) {
    std::string msg = "CHAT:" + playerName + ":" + message;
    broadcast(msg);
    std::lock_guard<std::mutex> lock(g_io_mutex);
    std::cout << "[CHAT] " << playerName << ": " << message << std::endl;
}

// ===== Deck & Cards =====
std::vector<Card> getFullDeck() {
    std::vector<Card> d;
    std::vector<std::string> s{"♥", "♦", "♣", "♠"};
    std::vector<std::string> r{"2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"};
    for (auto& suit : s) {
        for (auto& rank : r) {
            d.push_back({rank, suit});
        }
    }
    return d;
}

void createDeck() {
    deck = getFullDeck();
}

void shuffleDeck() {
    std::shuffle(deck.begin(), deck.end(), std::mt19937(std::random_device{}()));
}

Card drawCard() {
    Card c = deck.back();
    deck.pop_back();
    return c;
}

std::string displayCards(const std::vector<Card> &cards) {
    std::stringstream ss;
    for (int l = 0; l < 5; l++) {
        for (auto& c : cards) {
            if (l == 0) ss << "┌─────┐ ";
            else if (l == 1) ss << "│" << c.rank << (c.rank.size() == 1 ? "    │ " : "   │ ");
            else if (l == 2) ss << "│  " << c.suit << "  │ ";
            else if (l == 3) ss << "│" << (c.rank.size() == 1 ? "    " : "   ") << c.rank << "│ ";
            else ss << "└─────┘ ";
        }
        ss << "\n";
    }
    return ss.str();
}

// ===== Showdown Helpers (Full Evaluator) =====
int getCardValue(const std::string& r) {
    if (r == "A") return 14;
    if (r == "K") return 13;
    if (r == "Q") return 12;
    if (r == "J") return 11;
    return std::stoi(r);
}

std::string getRankName(int v) {
    if (v == 14) return "Ace";
    if (v == 13) return "King";
    if (v == 12) return "Queen";
    if (v == 11) return "Jack";
    if (v == 10) return "10";
    if (v <= 9) return std::to_string(v);
    return "?";
}

long long getKickerScore(const std::vector<int>& k) {
    long long s = 0;
    long long m = 100000000;
    for (int i : k) {
        s += i * m;
        m /= 100;
    }
    return s;
}

HandResult evaluate5CardHand(std::vector<Card>& h) {
    if (h.size() != 5) return {0, "Invalid"};

    std::sort(h.begin(), h.end(), [](const Card& a, const Card& b) {
        return getCardValue(a.rank) > getCardValue(b.rank);
    });

    std::vector<int> r;
    std::set<std::string> s;
    for (const auto& c : h) {
        r.push_back(getCardValue(c.rank));
        s.insert(c.suit);
    }

    bool f = (s.size() == 1);
    bool t = true;
    for (int i = 0; i < 4; ++i) {
        if (r[i] != r[i + 1] + 1) t = false;
    }
    
    // A-5 Straight check
    if (!t && r[0] == 14 && r[1] == 5 && r[2] == 4 && r[3] == 3 && r[4] == 2) {
        t = true;
        r = {5, 4, 3, 2, 1}; 
    }

    if (t && f) {
        if (r[0] == 14) return {static_cast<long long>(9e12), "a Royal Flush"};
        return {static_cast<long long>(8e12) + r[0], "a Straight Flush (" + getRankName(r[0]) + " high)"};
    }

    std::map<int, int> rc;
    for (int i : r) rc[i]++;
    
    int foak = 0, toak = 0;
    std::vector<int> p, k;
    for (auto const& [rk, ct] : rc) {
        if (ct == 4) foak = rk;
        else if (ct == 3) toak = rk;
        else if (ct == 2) p.push_back(rk);
        else k.push_back(rk);
    }
    
    std::sort(p.rbegin(), p.rend());
    std::sort(k.rbegin(), k.rend());

    if (foak > 0) {
        return {static_cast<long long>(7e12) + (foak * 100) + k[0], "Four of a Kind (" + getRankName(foak) + "s)"};
    }
    if (toak > 0 && p.size() > 0) {
        return {static_cast<long long>(6e12) + (toak * 100) + p[0], "a Full House (" + getRankName(toak) + "s full of " + getRankName(p[0]) + "s)"};
    }
    if (f) {
        return {static_cast<long long>(5e12) + getKickerScore(r), "a Flush (" + getRankName(r[0]) + " high)"};
    }
    if (t) {
        return {static_cast<long long>(4e12) + r[0], "a Straight (" + getRankName(r[0]) + " high)"};
    }
    if (toak > 0) {
        return {static_cast<long long>(3e12) + (toak * 10000) + (k[0] * 100) + k[1], "Three of a Kind (" + getRankName(toak) + "s)"};
    }
    if (p.size() >= 2) {
        return {static_cast<long long>(2e12) + (p[0] * 10000) + (p[1] * 100) + k[0], "Two Pair (" + getRankName(p[0]) + "s and " + getRankName(p[1]) + "s)"};
    }
    if (p.size() == 1) {
        return {static_cast<long long>(1e12) + (p[0] * 1000000) + (k[0] * 10000) + (k[1] * 100) + k[2], "a Pair of " + getRankName(p[0]) + "s"};
    }
    
    return {getKickerScore(r), "High Card " + getRankName(r[0])};
}

HandResult getFullPlayerHand(Player& p, const std::vector<Card>& simComCards) {
    std::vector<Card> all = p.hand;
    all.insert(all.end(), simComCards.begin(), simComCards.end());
    
    HandResult best = {0, "Nothing"};
    int n = all.size();
    
    if (n < 5) {
        if (p.hand.empty()) return {0, "Nothing"};
        int v1 = getCardValue(p.hand[0].rank);
        int v2 = getCardValue(p.hand[1].rank);
        if (v1 == v2) return {static_cast<long long>(1e12) + v1, "a Pair of " + getRankName(v1) + "s"};
        return {std::max(v1, v2), "High Card " + getRankName(std::max(v1, v2))};
    }
    
    std::vector<bool> v(n);
    std::fill(v.begin() + (n - 5), v.end(), false);
    std::fill(v.begin(), v.begin() + 5, true);
    std::sort(v.rbegin(), v.rend());
    
    do {
        std::vector<Card> cur5;
        for (int i = 0; i < n; ++i) {
            if (v[i]) {
                cur5.push_back(all[i]);
            }
        }
        HandResult curRes = evaluate5CardHand(cur5);
        if (curRes.rank > best.rank) {
            best = curRes;
        }
    } while (std::prev_permutation(v.begin(), v.end()));
    
    return best;
}

// ===== Monte Carlo Simulator =====
double runMonteCarlo(Player& ai, const std::vector<Card>& mainDeck) {
    int wins = 0;
    int ties = 0;
    std::vector<Card> aiHand = ai.hand;
    
    std::vector<Card> simDeck = getFullDeck();
    simDeck.erase(std::remove(simDeck.begin(), simDeck.end(), aiHand[0]), simDeck.end());
    simDeck.erase(std::remove(simDeck.begin(), simDeck.end(), aiHand[1]), simDeck.end());
    for (const auto& card : communityCards) {
        simDeck.erase(std::remove(simDeck.begin(), simDeck.end(), card), simDeck.end());
    }
    
    for (int i = 0; i < MONTE_CARLO_SIMULATIONS; ++i) {
        std::vector<Card> simDeckThisRound = simDeck;
        std::shuffle(simDeckThisRound.begin(), simDeckThisRound.end(), std::mt19937(std::random_device{}()));
        
        Player simOpponent;
        simOpponent.hand.push_back(simDeckThisRound.back());
        simDeckThisRound.pop_back();
        simOpponent.hand.push_back(simDeckThisRound.back());
        simDeckThisRound.pop_back();
        
        std::vector<Card> simCommunityCards = communityCards;
        int cardsToDeal = 5 - simCommunityCards.size();
        
        for (int j = 0; j < cardsToDeal; ++j) {
            if (simDeckThisRound.empty()) break;
            simCommunityCards.push_back(simDeckThisRound.back());
            simDeckThisRound.pop_back();
        }
        
        HandResult botHand = getFullPlayerHand(ai, simCommunityCards);
        HandResult oppHand = getFullPlayerHand(simOpponent, simCommunityCards);
        
        if (botHand.rank > oppHand.rank) wins++;
        else if (botHand.rank == oppHand.rank) ties++;
    }
    return (wins + (ties / 2.0)) / MONTE_CARLO_SIMULATIONS;
}

// ===== REVISED: AI LOGIC (Hybrid: MCS + Opponent Model + Bluffing) =====
std::string AIAction(Player &ai, int roundNumber, const std::vector<Card>& mainDeck) {
    int callAmt = currentBet - ai.currentBet;
    
    Player* opponent = getHumanOpponent();
    double oppVPIP = 0.0, oppPFR = 0.0;
    bool oppIsTight = false, oppIsAggressive = false;
    
    if (opponent && opponent->handsPlayed > 10) {
        oppVPIP = (double)opponent->vpipActions / opponent->handsPlayed;
        oppPFR = (double)opponent->pfrActions / opponent->handsPlayed;
        oppIsTight = (oppVPIP < 0.20);
        oppIsAggressive = (oppPFR > 0.15);
    }
    
    double potOdds = (pot + callAmt > 0) ? (double)callAmt / (double)(pot + callAmt) : 0.0;
    
    // --- NEW: Thinking Animation ---
    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << "AI_Bot is thinking    " << std::flush;
        for (int i = 0; i < 3; ++i) { // Adjust loop count for desired duration
            std::cout << "\b\b\b.  " << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::cout << "\b\b\b.. " << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::cout << "\b\b\b..." << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::cout << "\r" << std::string(30, ' ') << "\r"; // Clear line
    }
    
    double equity = runMonteCarlo(ai, mainDeck);
    
    bool hasFlushDraw = false, hasOESD = false, hasGutshot = false;
    std::vector<Card> curH = ai.hand;
    curH.insert(curH.end(), communityCards.begin(), communityCards.end());
    
    if (curH.size() >= 4) {
        std::map<std::string, int> sc;
        for (const auto& c : curH) sc[c.suit]++;
        for (auto const& [s, c] : sc) if (c == 4) hasFlushDraw = true;
        
        std::set<int> ur;
        for (const auto& c : curH) ur.insert(getCardValue(c.rank));
        if (ur.count(14)) ur.insert(1); // Ace for low straight
        
        for (int r : ur) {
            if (ur.count(r + 1) && ur.count(r + 2) && ur.count(r + 3)) {
                hasOESD = true;
                break;
            }
        }
        if (!hasOESD) {
            for (int r : ur) {
                if ((ur.count(r + 1) && ur.count(r + 3) && ur.count(r + 4)) || // e.g., 5,6, 8,9
                    (r == 1 && ur.count(2) && ur.count(3) && ur.count(5)) || // A,2,3, 5
                    (r == 11 && ur.count(12) && ur.count(13) && ur.count(14))) // J,Q,K,A
                {
                    hasGutshot = true;
                    break;
                }
            }
        }
    }
    bool strongDraw = hasFlushDraw || hasOESD;
    
    double requiredEquity = potOdds;
    if (callAmt > 0) {
        if (oppIsTight && !oppIsAggressive) requiredEquity *= 1.25;
        else if (!oppIsTight && oppIsAggressive) requiredEquity *= 0.85;
    }
    
    if (strongDraw && callAmt > 0 && callAmt < pot / 2.0) requiredEquity *= 0.75;
    else if (hasGutshot && callAmt > 0 && callAmt < pot / 3.0) requiredEquity *= 0.90;
    
    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << "AI Debug: E=" << (equity * 100) << "%|Need=" << (potOdds * 100) << "%|AdjNeed=" << (requiredEquity * 100) << "%" << std::endl;
        if (opponent && opponent->handsPlayed > 10) {
            std::cout << "AI Debug: Opp VPIP=" << (oppVPIP * 100) << "% PFR=" << (oppPFR * 100) << "%(T=" << oppIsTight << ",A=" << oppIsAggressive << ")" << std::endl;
        }
        if (strongDraw) std::cout << "AI Debug: Strong Draw." << std::endl;
        else if (hasGutshot) std::cout << "AI Debug: Gutshot." << std::endl;
    }
    
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(1, 100);
    
    if (callAmt == 0) {
        // --- Bluffing Logic (10% chance on turn/river if checked to) ---
        if ((roundNumber == 2 || roundNumber == 3) && opponent != nullptr && dist(rng) <= 10) {
            int bAmt = pot / 2;
            if (bAmt < 50) bAmt = 50;
            if (bAmt > ai.chips) bAmt = ai.chips;
            if (bAmt <= 0) return "CHECK";
            {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << "AI Debug: Bluff bet." << std::endl;
            }
            return "RAISE " + std::to_string(bAmt);
        }
        
        // --- Value Betting ---
        if (equity > 0.6 || strongDraw) {
            int bAmt = pot / 2;
            if (bAmt < 50) bAmt = 50;
            if (bAmt > ai.chips) bAmt = ai.chips;
            if (bAmt <= 0) return "CHECK";
            return "RAISE " + std::to_string(bAmt);
        } else {
            return "CHECK";
        }
    } else {
        if (equity > requiredEquity) {
            // --- Semi-bluff Raise (20% chance with strong draw) ---
            if (strongDraw && dist(rng) <= 20) {
                int rAmt = callAmt * 2 + pot;
                if (rAmt > ai.chips) rAmt = ai.chips;
                if (rAmt <= callAmt) return "CALL";
                {
                    std::lock_guard<std::mutex> lock(g_io_mutex);
                    std::cout << "AI Debug: Semi-bluff raise." << std::endl;
                }
                return "RAISE " + std::to_string(rAmt);
            }
            
            // --- Value Raise ---
            if (equity > 0.85 && !strongDraw) {
                int rAmt = callAmt * 2 + pot;
                if (rAmt > ai.chips) rAmt = ai.chips;
                if (rAmt <= callAmt) return "CALL";
                return "RAISE " + std::to_string(rAmt);
            }
            return "CALL";
        } else {
            {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << "AI Debug: Folding. E " << (equity * 100) << "% < Req " << (requiredEquity * 100) << "%." << std::endl;
            }
            return "FOLD";
        }
    }
}


// ===== Table Display =====
void showTable() {
    clearScreen(); // NEW: Clear screen
    std::stringstream ss;
    
    // --- NEW: Player Table Formatting ---
    ss << "\n";
    {
        std::lock_guard<std::mutex> lock(g_players_mutex);
        ss << "┌───────────────────┬──────────────┬──────────┐\n";
        ss << "│ Player            │ Chips        │ Status   │\n";
        ss << "├───────────────────┼──────────────┼──────────┤\n";
        for (auto &p : players) {
            char buffer[100];
            std::string status = "ACTIVE";
            if (!p.isConnected) status = "OFFLINE";
            else if (p.folded) status = "FOLDED";
            else if (p.allIn) status = "ALL-IN";
            
            snprintf(buffer, 100, "│ %-17s │ %-12d │ %-8s │", p.name.substr(0, 17).c_str(), p.chips, status.c_str()); // Limit name length
            ss << buffer << "\n";
        }
        ss << "└───────────────────┴──────────────┴──────────┘\n";
        ss << "Pot: " << pot << "\n"; // Show pot below table
    }
    
    broadcast(ss.str());
    
    if (communityCards.size() > 0) {
        std::stringstream ss_cards;
        ss_cards << "CARDS";
        for (auto& c : communityCards) {
            ss_cards << " " << c.toString();
        }
        broadcast(ss_cards.str());
    }
    
    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << ss.str();
        if (communityCards.size() > 0) {
            std::cout << "Community Cards:\n" << displayCards(communityCards);
        }
    }
}

// ===== Reset Function =====
void resetForNextHand() {
    std::lock_guard<std::mutex> lock(g_players_mutex);
    pot = 0;
    currentBet = 0;
    communityCards.clear();
    g_preFlopRaiseMade = false;
    
    for (auto it = players.begin(); it != players.end();) {
        if (!it->isConnected || it->chips <= 0) {
            if (!it->isAI) {
                std::cout << it->name << " removed." << std::endl;
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
void handleIncomingMessage(int s, const std::string& d) {
    Player* p = getPlayerBySocket(s);
    if (!p) return;
    
    if (d == "DISCONNECTED") {
        p->isConnected = false;
        p->folded = true;
        broadcast(p->name + " disconnected.");
    } else if (d.find("CHAT:") == 0) {
        broadcastChat(p->name, d.substr(5));
    }
}

// ===== Player Input =====
std::string getPlayerInput(Player &p) {
    sendToPlayer(p, "YOUR_MOVE");
    while (true) {
        Message msg;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_inbound_mutex);
            if (!g_inbound_messages.empty()) {
                msg = g_inbound_messages.front();
                g_inbound_messages.pop();
                found = true;
            }
        }
        
        if (found) {
            if (msg.socket == p.socket) {
                if (msg.data.find("CHAT:") == 0) {
                    handleIncomingMessage(msg.socket, msg.data);
                    continue; // Loop again for a move
                } else if (msg.data == "DISCONNECTED") {
                    handleIncomingMessage(msg.socket, msg.data);
                    return "FOLD";
                }
                return msg.data;
            } else {
                // Message from another player, handle it and continue waiting
                handleIncomingMessage(msg.socket, msg.data);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ===== Betting Round =====
void bettingRound(int roundNumber) {
    int raises = 0;
    currentBet = 0;
    for (auto& p : players) {
        p.currentBet = 0;
    }
    
    int turn = 0;
    bool opened = false;
    
    while (true) {
        int active = 0;
        for (auto& p : players) {
            if (!p.folded && !p.allIn && p.isConnected) {
                active++;
            }
        }
        if (active <= 1) break;
        
        Player& p = players[turn % players.size()];
        
        if (!p.folded && !p.allIn && p.isConnected) {
            showTable();
            std::string action;
            
            if (p.isAI) {
                action = AIAction(p, roundNumber, deck);
            } else {
                action = getPlayerInput(p);
            }
            
            int callAmt = currentBet - p.currentBet;
            bool voluntary = false;
            bool isRaise = false;
            
            if (action.find("FOLD") != std::string::npos) {
                p.folded = true;
                broadcast(p.name + " folds.");
            } else if (action.find("CHECK") != std::string::npos) {
                if (callAmt == 0) {
                    broadcast(p.name + " checks.");
                } else {
                    broadcast(p.name + " folded.");
                    p.folded = true;
                }
            } else if (action.find("CALL") != std::string::npos) {
                if (callAmt == 0) {
                    broadcast(p.name + " checks.");
                } else {
                    if (callAmt >= p.chips) {
                        callAmt = p.chips;
                        p.allIn = true;
                        broadcast(p.name + " calls ALL-IN!");
                    } else {
                        broadcast(p.name + " calls " + std::to_string(callAmt) + ".");
                    }
                    p.chips -= callAmt;
                    pot += callAmt;
                    p.currentBet += callAmt;
                    voluntary = true;
                }
            } else if (action.find("RAISE") != std::string::npos) {
                int rAmt = 0;
                try {
                    rAmt = std::stoi(action.substr(action.find(" ") + 1));
                } catch (...) {
                    rAmt = 50; // Default raise if parse fails
                }
                
                int total = currentBet + rAmt;
                int putIn = total - p.currentBet;
                
                if (putIn >= p.chips) {
                    putIn = p.chips;
                    total = p.currentBet + putIn;
                    p.allIn = true;
                    broadcast(p.name + " raises ALL-IN!");
                } else {
                    broadcast(p.name + " raises " + std::to_string(rAmt) + ".");
                }
                
                p.chips -= putIn;
                pot += putIn;
                p.currentBet = total;
                currentBet = total;
                raises++;
                voluntary = true;
                isRaise = true;
            } else {
                broadcast(p.name + " folded.");
                p.folded = true;
            }
            
            // --- Stat Tracking ---
            if (roundNumber == 0 && !p.isAI) {
                if (voluntary) p.vpipActions++;
                if (isRaise && !g_preFlopRaiseMade) {
                    p.pfrActions++;
                    g_preFlopRaiseMade = true;
                }
            }
            
            if (isRaise || (callAmt > 0 && action.find("CALL") != std::string::npos)) {
                opened = true;
            }
        }
        
        turn++;
        
        // --- Check for end of round ---
        bool finished = true;
        int last = -1;
        int activeIn = 0;
        for (auto& pl : players) {
            if (pl.folded || !pl.isConnected) continue;
            if (!pl.allIn) {
                if (last == -1) last = pl.currentBet;
                if (pl.currentBet != last) finished = false;
                activeIn++;
            }
        }
        
        if (activeIn > 0 && finished && turn >= players.size()) break;
        if (active <= 1) break;
    }
    
    for (auto& p : players) {
        p.currentBet = 0;
    }
}

// ===== Client Handler =====
void clientHandler(int sock) {
    char buf[1024];
    std::string netBuf = "";
    try {
        while (true) {
            int vr = read(sock, buf, 1023);
            if (vr <= 0) {
                std::lock_guard<std::mutex> lock(g_inbound_mutex);
                g_inbound_messages.push({sock, "DISCONNECTED"});
                break;
            }
            buf[vr] = '\0';
            netBuf += buf;
            
            size_t pos;
            while ((pos = netBuf.find('\n')) != std::string::npos) {
                std::string msg = netBuf.substr(0, pos);
                netBuf.erase(0, pos + 1);
                msg.erase(std::remove(msg.begin(), msg.end(), '\r'), msg.end());
                
                std::lock_guard<std::mutex> lock(g_inbound_mutex);
                g_inbound_messages.push({sock, msg});
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cerr << "Ex clientHandler:" << e.what() << std::endl;
    }
    close(sock);
}

// ===== Check if Hand Over =====
bool checkIfHandOver() {
    int active = 0;
    Player* winner = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_players_mutex);
        for (auto& p : players) {
            if (!p.folded && p.isConnected) {
                active++;
                winner = &p;
            }
        }
    }
    
    if (active <= 1 && winner != nullptr) {
        std::string msg = winner->name + " wins " + std::to_string(pot) + " (last standing)!";
        broadcast(msg);
        {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            std::cout << msg << std::endl;
        }
        winner->chips += pot;
        return true;
    }
    return false;
}

// ===== Main =====
int main() {
    clearScreen(); // Initial clear
    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << "AI player? (y/n):";
        char c;
        std::cin >> c;
        std::cin.ignore(); // Consume newline
        if (c == 'y' || c == 'Y') {
            Player ai;
            ai.name = "AI_Bot";
            ai.isAI = true;
            players.push_back(ai);
            std::cout << "AI joined.\n";
        }
    }
    
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);
    
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, MAX_PLAYERS);
    
    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << "Server started on port " << PORT << ". Waiting...\n";
    }
    
    // --- Connection Accepting Thread ---
    std::thread([&]() {
        while (true) {
            int sock = accept(server_fd, (struct sockaddr*)&address, &addrlen);
            if (sock < 0) continue;
            
            char b[1024] = {0};
            int vr = read(sock, b, 1024);
            if (vr <= 0) {
                close(sock);
                continue;
            }
            
            std::string pn(b, vr);
            pn.erase(std::remove(pn.begin(), pn.end(), '\n'), pn.end());
            pn.erase(std::remove(pn.begin(), pn.end(), '\r'), pn.end());
            
            {
                std::lock_guard<std::mutex> lock(g_players_mutex);
                if (players.size() >= MAX_PLAYERS) {
                    send(sock, "SERVER_FULL\n", 12, 0);
                    close(sock);
                    continue;
                }
                Player p;
                p.name = pn;
                p.socket = sock;
                players.push_back(p);
                send(sock, ("WELCOME " + pn + "\n").c_str(), pn.size() + 9, 0);
                std::cout << pn << " connected.\n";
            }
            std::thread(clientHandler, sock).detach();
        }
    }).detach();
    
    // --- Admin 'start' loop ---
    std::string command;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(g_players_mutex);
            std::cout << "\nPlayers(" << players.size() << "/" << MAX_PLAYERS << "):";
            for (auto &p : players) std::cout << p.name << " ";
            std::cout << std::endl;
        }
        std::cout << "Type 'start':";
        std::getline(std::cin, command);
        if (command == "start") {
            if (players.size() >= 2) break;
            std::cout << "Need >= 2 players.\n";
        }
    }

    // ===== Main Game Loop =====
    while (true) {
        resetForNextHand();
        
        if (players.size() < 2) {
            std::cout << "Not enough players.\n";
            broadcast("Not enough players.");
            break;
        }
        
        // --- Ante ---
        {
            std::lock_guard<std::mutex> lock(g_players_mutex);
            {
                std::stringstream ss;
                ss << "Collecting ante of " << ANTE_AMOUNT;
                {
                    std::lock_guard<std::mutex> io(g_io_mutex);
                    std::cout << ss.str() << std::endl;
                }
                broadcast_unsafe(ss.str());
            }
            for (auto& p : players) {
                if (p.isConnected) {
                    int a = std::min(ANTE_AMOUNT, p.chips);
                    p.chips -= a;
                    pot += a;
                    if (p.chips == 0 && a > 0) {
                        p.allIn = true;
                        broadcast_unsafe(p.name + " is all-in from ante.");
                    }
                }
            }
            {
                std::stringstream ss;
                ss << "Pot starts at " << pot;
                {
                    std::lock_guard<std::mutex> io(g_io_mutex);
                    std::cout << ss.str() << std::endl;
                }
                broadcast_unsafe(ss.str());
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(g_players_mutex);
            for (auto& p : players) p.handsPlayed++;
        }
        
        broadcast("GAME_STARTING");
        
        // --- Deal Hole Cards ---
        for (auto &p : players) {
            if (p.hand.empty()) {
                p.hand.push_back(drawCard());
                p.hand.push_back(drawCard());
                if (!p.isAI) {
                    sendToPlayer(p, "HOLE " + p.hand[0].toString() + " " + p.hand[1].toString());
                } else {
                    std::lock_guard<std::mutex> lock(g_io_mutex);
                    std::cout << "AI hole cards:\n" << displayCards(p.hand);
                }
            }
        }
        
        // --- Betting Rounds ---
        bettingRound(0); // Pre-flop
        if (checkIfHandOver()) continue;
        
        for (int i = 0; i < 3; i++) communityCards.push_back(drawCard()); // Flop
        showTable();
        bettingRound(1); // Post-flop
        if (checkIfHandOver()) continue;
        
        communityCards.push_back(drawCard()); // Turn
        showTable();
        bettingRound(2); // Post-turn
        if (checkIfHandOver()) continue;
        
        communityCards.push_back(drawCard()); // River
        showTable();
        bettingRound(3); // Post-river
        if (checkIfHandOver()) continue;

        broadcast("\n--- SHOWDOWN ---");
        {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            std::cout << "\n--- SHOWDOWN ---" << std::endl;
        }
        
        // ===== UPDATED: SHOWDOWN LOGIC FOR SPLIT POTS =====
        std::vector<Player*> winners;
        HandResult bestHand = {0, "Nothing"};

        {
            std::lock_guard<std::mutex> lock(g_players_mutex);
            for (auto& p : players) {
                if (!p.folded && p.isConnected) {
                    std::string bcHand = p.name + "'s hand: " + p.hand[0].toString() + " " + p.hand[1].toString();
                    broadcast_unsafe(bcHand);
                    std::string coutHand = p.name + "'s hand: " + p.hand[0].rank + p.hand[0].suit + " " + p.hand[1].rank + p.hand[1].suit;
                    {
                        std::lock_guard<std::mutex> io(g_io_mutex);
                        std::cout << coutHand << std::endl;
                    }
                    
                    HandResult hand = getFullPlayerHand(p, communityCards);
                    
                    if (hand.rank > bestHand.rank) {
                        bestHand = hand;
                        winners.clear(); // New best hand, clear old winners
                        winners.push_back(&p);
                    } else if (hand.rank == bestHand.rank && bestHand.rank > 0) {
                        winners.push_back(&p); // Tied for best hand
                    }
                }
            }
        }

        if (!winners.empty()) {
            std::string msg;
            if (winners.size() == 1) {
                // Single winner
                Player* winner = winners[0];
                msg = winner->name + " wins " + std::to_string(pot) + " with " + bestHand.name + "!";
                winner->chips += pot;
            } else {
                // Split pot
                int splitAmount = pot / winners.size();
                int remainder = pot % winners.size();
                std::string winnerNames;
                for (size_t i = 0; i < winners.size(); ++i) {
                    winnerNames += winners[i]->name;
                    if (i < winners.size() - 1) winnerNames += ", ";
                    winners[i]->chips += splitAmount;
                }
                winners[0]->chips += remainder; // Give remainder to first winner
                msg = "Split pot! " + std::to_string(pot) + " split between: " + winnerNames + " with " + bestHand.name;
            }
            broadcast(msg);
            {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << msg << std::endl;
            }
        } else {
            std::string msg = "No winner, pot returned (NI).";
            broadcast(msg);
            {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << msg << std::endl;
            }
        }
        // ===== END UPDATED SHOWDOWN LOGIC =====

        std::string choice;
        {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            std::cout << "--- Hand Over ---\nAnother round? (y/n):";
        }
        broadcast("HAND_OVER\nWaiting for admin...");
        std::cin.clear();
        std::getline(std::cin, choice);
        
        if (choice.empty() || (choice[0] != 'y' && choice[0] != 'Y')) {
            broadcast("Game ending.");
            {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                std::cout << "Shutting down.\n";
            }
            break;
        }
    }
    
    std::cout << "Game Over.\n";
    return 0;
}

