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

    // --- NEW: Opponent Modeling Stats ---
    int handsPlayed = 0; // Total hands dealt to this player
    int vpipActions = 0; // Times voluntarily put money in pre-flop (call/raise)
    int pfrActions = 0;  // Times raised pre-flop

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
// --- NEW: Track pre-flop raise status ---
bool g_preFlopRaiseMade = false;


// ===== Utility Functions =====
Player* getPlayerBySocket(int socket) {
    for (auto &p : players) {
        if (p.socket == socket) return &p;
    }
    return nullptr;
}

// Get the main human opponent (assumes only one AI)
Player* getHumanOpponent() {
    for (auto& p : players) {
        if (!p.isAI && !p.folded && p.isConnected) {
            return &p;
        }
    }
    return nullptr; // No active human opponent
}


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
    broadcast_unsafe(msg);
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
std::vector<Card> getFullDeck() {
    std::vector<Card> fullDeck;
    std::vector<std::string> suits{"♥","♦","♣","♠"};
    std::vector<std::string> ranks{"2","3","4","5","6","7","8","9","10","J","Q","K","A"};
    for(auto &s:suits) for(auto &r:ranks) fullDeck.push_back(Card{r,s});
    return fullDeck;
}

void createDeck(){
    deck = getFullDeck();
}

void shuffleDeck(){ std::shuffle(deck.begin(),deck.end(),std::mt19937(std::random_device{}())); }
Card drawCard(){ Card c=deck.back(); deck.pop_back(); return c; }

std::string displayCards(const std::vector<Card> &cards){
    std::stringstream ss;
    for(int line=0;line<5;line++){
        for(auto &c:cards){
            if(line==0) ss<<"┌─────┐ ";
            else if(line==1) ss<<"│"<<c.rank<<(c.rank.size()==1?"    │ ":"   │ ");
            else if(line==2) ss<<"│  "<<c.suit<<"  │ ";
            else if(line==3) ss<<"│"<<(c.rank.size()==1?"    ":"   ")<<c.rank<<"│ ";
            else ss<<"└─────┘ ";
        }
        ss<<"\n";
    }
    return ss.str();
}

// ===== Showdown Helpers (Full Evaluator) =====

int getCardValue(const std::string& rank) {
    if (rank == "A") return 14; if (rank == "K") return 13; if (rank == "Q") return 12;
    if (rank == "J") return 11; return std::stoi(rank);
}

std::string getRankName(int val) {
    if (val == 14) return "Ace"; if (val == 13) return "King"; if (val == 12) return "Queen";
    if (val == 11) return "Jack"; if (val == 10) return "10"; if (val <= 9) return std::to_string(val);
    return "?";
}

long long getKickerScore(const std::vector<int>& kickers) {
    long long score = 0; long long multiplier = 100000000; // 10^8
    for(int kicker : kickers) { score += kicker * multiplier; multiplier /= 100; } return score;
}

HandResult evaluate5CardHand(std::vector<Card>& hand) {
    if (hand.size() != 5) return {0, "Invalid Hand"};
    std::sort(hand.begin(), hand.end(), [](const Card& a, const Card& b) { return getCardValue(a.rank) > getCardValue(b.rank); });
    std::vector<int> ranks; std::set<std::string> suits;
    for(const auto& c : hand) { ranks.push_back(getCardValue(c.rank)); suits.insert(c.suit); }
    bool isFlush = (suits.size() == 1); bool isStraight = true;
    for(int i = 0; i < 4; ++i) { if (ranks[i] != ranks[i+1] + 1) isStraight = false; }
    if (!isStraight && ranks[0] == 14 && ranks[1] == 5 && ranks[2] == 4 && ranks[3] == 3 && ranks[4] == 2) { isStraight = true; ranks = {5, 4, 3, 2, 1}; }
    if (isStraight && isFlush) { if (ranks[0] == 14) return {9000000000000, "a Royal Flush"}; return {8000000000000 + ranks[0], "a Straight Flush (" + getRankName(ranks[0]) + " high)"}; }
    std::map<int, int> rankCounts; for(int r : ranks) rankCounts[r]++;
    int fourOfAKindRank = 0; int threeOfAKindRank = 0; std::vector<int> pairs; std::vector<int> kickers;
    for (auto const& [rank, count] : rankCounts) { if (count == 4) fourOfAKindRank = rank; else if (count == 3) threeOfAKindRank = rank; else if (count == 2) pairs.push_back(rank); else kickers.push_back(rank); }
    std::sort(pairs.rbegin(), pairs.rend()); std::sort(kickers.rbegin(), kickers.rend());
    if (fourOfAKindRank > 0) { long long score = 7000000000000 + (fourOfAKindRank * 100) + kickers[0]; return {score, "Four of a Kind (" + getRankName(fourOfAKindRank) + "s)"}; }
    if (threeOfAKindRank > 0 && pairs.size() > 0) { long long score = 6000000000000 + (threeOfAKindRank * 100) + pairs[0]; return {score, "a Full House (" + getRankName(threeOfAKindRank) + "s full of " + getRankName(pairs[0]) + "s)"}; }
    if (isFlush) { return {5000000000000 + getKickerScore(ranks), "a Flush (" + getRankName(ranks[0]) + " high)"}; }
    if (isStraight) { return {4000000000000 + ranks[0], "a Straight (" + getRankName(ranks[0]) + " high)"}; }
    if (threeOfAKindRank > 0) { long long score = 3000000000000 + (threeOfAKindRank * 10000) + (kickers[0] * 100) + kickers[1]; return {score, "Three of a Kind (" + getRankName(threeOfAKindRank) + "s)"}; }
    if (pairs.size() >= 2) { long long score = 2000000000000 + (pairs[0] * 10000) + (pairs[1] * 100) + kickers[0]; return {score, "Two Pair (" + getRankName(pairs[0]) + "s and " + getRankName(pairs[1]) + "s)"}; }
    if (pairs.size() == 1) { long long score = 1000000000000 + (pairs[0] * 1000000) + (kickers[0] * 10000) + (kickers[1] * 100) + kickers[2]; return {score, "a Pair of " + getRankName(pairs[0]) + "s"}; }
    return {getKickerScore(ranks), "High Card " + getRankName(ranks[0])};
}

HandResult getFullPlayerHand(Player& p, const std::vector<Card>& simCommunityCards) {
    std::vector<Card> allCards = p.hand;
    allCards.insert(allCards.end(), simCommunityCards.begin(), simCommunityCards.end());
    HandResult bestHand = {0, "Nothing"}; int n = allCards.size();
    if (n < 5) { if (p.hand.empty()) return {0, "Nothing"}; int v1=getCardValue(p.hand[0].rank), v2=getCardValue(p.hand[1].rank); if (v1==v2) return {1000000000000+v1, "a Pair of "+getRankName(v1)+"s"}; return {std::max(v1,v2), "High Card "+getRankName(std::max(v1,v2))}; }
    std::vector<bool> v(n); std::fill(v.begin() + (n - 5), v.end(), false); std::fill(v.begin(), v.begin() + 5, true); std::sort(v.rbegin(), v.rend());
    do { std::vector<Card> current5CardHand; for (int i = 0; i < n; ++i) { if (v[i]) { current5CardHand.push_back(allCards[i]); } } HandResult currentResult = evaluate5CardHand(current5CardHand); if (currentResult.rank > bestHand.rank) { bestHand = currentResult; } } while (std::prev_permutation(v.begin(), v.end()));
    return bestHand;
}

// ===== Monte Carlo Simulator =====
double runMonteCarlo(Player& ai, const std::vector<Card>& mainDeck) {
    int wins = 0; int ties = 0; std::vector<Card> aiHand = ai.hand;
    std::vector<Card> simDeck = getFullDeck();
    simDeck.erase(std::remove(simDeck.begin(), simDeck.end(), aiHand[0]), simDeck.end());
    simDeck.erase(std::remove(simDeck.begin(), simDeck.end(), aiHand[1]), simDeck.end());
    for (const auto& card : communityCards) { simDeck.erase(std::remove(simDeck.begin(), simDeck.end(), card), simDeck.end()); }
    for (int i = 0; i < MONTE_CARLO_SIMULATIONS; ++i) {
        std::vector<Card> simDeckThisRound = simDeck; std::shuffle(simDeckThisRound.begin(), simDeckThisRound.end(), std::mt19937(std::random_device{}()));
        Player simOpponent; simOpponent.hand.push_back(simDeckThisRound.back()); simDeckThisRound.pop_back(); simOpponent.hand.push_back(simDeckThisRound.back()); simDeckThisRound.pop_back();
        std::vector<Card> simCommunityCards = communityCards; int cardsToDeal = 5 - simCommunityCards.size();
        for(int j=0; j < cardsToDeal; ++j) { if(simDeckThisRound.empty()) break; simCommunityCards.push_back(simDeckThisRound.back()); simDeckThisRound.pop_back(); }
        HandResult botHand = getFullPlayerHand(ai, simCommunityCards); HandResult oppHand = getFullPlayerHand(simOpponent, simCommunityCards);
        if (botHand.rank > oppHand.rank) wins++; else if (botHand.rank == oppHand.rank) ties++;
    }
    return (wins + (ties / 2.0)) / MONTE_CARLO_SIMULATIONS;
}

// ===== REVISED: AI LOGIC (Hybrid: MCS + Opponent Model + Bluffing) =====
std::string AIAction(Player &ai, int roundNumber, const std::vector<Card>& mainDeck){
    int callAmt = currentBet - ai.currentBet;

    // --- 0. Get Opponent Model ---
    Player* opponent = getHumanOpponent();
    double oppVPIP = 0.0, oppPFR = 0.0;
    bool oppIsTight = false, oppIsAggressive = false;
    if (opponent && opponent->handsPlayed > 10) { // Need some data first
        oppVPIP = (double)opponent->vpipActions / opponent->handsPlayed;
        oppPFR = (double)opponent->pfrActions / opponent->handsPlayed;
        oppIsTight = (oppVPIP < 0.20); // VPIP < 20% = Tight
        oppIsAggressive = (oppPFR > 0.15); // PFR > 15% = Aggressive
    }

    // --- 1. Calculate Pot Odds ---
    double potOdds = 0.0;
    if (pot + callAmt > 0) {
        potOdds = (double)callAmt / (double)(pot + callAmt);
    }

    // --- 2. Calculate Equity via Monte Carlo ---
    { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << "AI_Bot is thinking..." << std::endl; }
    double equity = runMonteCarlo(ai, mainDeck);

    // --- 3. Check for Strong Draws ---
    bool hasFlushDraw = false, hasOESD = false, hasGutshot = false; // OESD = Open Ended Straight Draw
    std::vector<Card> currentHand = ai.hand; currentHand.insert(currentHand.end(), communityCards.begin(), communityCards.end());
    if (currentHand.size() >= 4) {
        std::map<std::string, int> suitCounts; for (const auto& c:currentHand) suitCounts[c.suit]++; for(auto const& [s,c]:suitCounts) if(c==4) hasFlushDraw=true;
        std::set<int> uniqueRanks; for(const auto& c:currentHand) uniqueRanks.insert(getCardValue(c.rank)); if(uniqueRanks.count(14)) uniqueRanks.insert(1);
        for(int r:uniqueRanks) if(uniqueRanks.count(r+1) && uniqueRanks.count(r+2) && uniqueRanks.count(r+3)) { hasOESD=true; break; }
        if(!hasOESD) for(int r:uniqueRanks) { if((uniqueRanks.count(r+1)&&uniqueRanks.count(r+3)&&uniqueRanks.count(r+4)) || (r==1&&uniqueRanks.count(2)&&uniqueRanks.count(3)&&uniqueRanks.count(5)) || (r==11&&uniqueRanks.count(12)&&uniqueRanks.count(13)&&uniqueRanks.count(14))) { hasGutshot=true; break; } }
    }
    bool strongDraw = hasFlushDraw || hasOESD;

    // --- 4. Adjust Required Equity based on Opponent Model ---
    double requiredEquity = potOdds;
    if (callAmt > 0) { // Only adjust if facing a bet
        if (oppIsTight && !oppIsAggressive) requiredEquity *= 1.25; // Need stronger hand vs tight/passive
        else if (!oppIsTight && oppIsAggressive) requiredEquity *= 0.85; // Can call wider vs loose/aggressive
    }

    // --- 5. Add Implied Odds Boost for Draws ---
    if (strongDraw && callAmt > 0 && callAmt < pot / 2.0) requiredEquity *= 0.75;
    else if (hasGutshot && callAmt > 0 && callAmt < pot / 3.0) requiredEquity *= 0.90;


    { // Log debug info
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << "AI Debug: Equity=" << (equity * 100.0) << "% | PotOddsNeed=" << (potOdds * 100.0) << "% | AdjustedNeed=" << (requiredEquity * 100.0) << "%" << std::endl;
        if(opponent && opponent->handsPlayed > 10) std::cout << "AI Debug: Opponent VPIP=" << (oppVPIP*100) << "% PFR=" << (oppPFR*100) << "% (Tight="<<oppIsTight<<", Aggro="<<oppIsAggressive<<")" << std::endl;
        if(strongDraw) std::cout << "AI Debug: Has Strong Draw." << std::endl; else if(hasGutshot) std::cout << "AI Debug: Has Gutshot." << std::endl;
    }

    // --- 6. Make Decision ---
    std::mt19937 rng(std::random_device{}()); // For randomness
    std::uniform_int_distribution<int> dist(1, 100);

    if (callAmt == 0) { // --- Bot can check ---
        // Pure Bluff Logic: 10% chance on Turn/River if heads-up
        if ((roundNumber == 2 || roundNumber == 3) && opponent != nullptr && dist(rng) <= 10) {
            int bluffAmt = pot / 2; if (bluffAmt < 50) bluffAmt = 50; if (bluffAmt > ai.chips) bluffAmt = ai.chips; if (bluffAmt <= 0) return "CHECK";
             { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << "AI Debug: Making pure bluff bet." << std::endl; }
            return "RAISE " + std::to_string(bluffAmt);
        }

        // Value Bet Logic
        if (equity > 0.6 || strongDraw) { // Bet if good equity OR a strong draw
            int betAmt = pot / 2; if (betAmt < 50) betAmt = 50; if (betAmt > ai.chips) betAmt = ai.chips; if (betAmt <= 0) return "CHECK";
            return "RAISE " + std::to_string(betAmt);
        } else {
            return "CHECK";
        }
    } else { // --- Bot is facing a bet ---
        if (equity > requiredEquity) { // --- Profitable to continue ---
             // Semi-Bluff Raise Logic: 20% chance with strong draw
             if (strongDraw && dist(rng) <= 20) {
                 int raiseAmt = callAmt * 2 + pot; if (raiseAmt > ai.chips) raiseAmt = ai.chips; if (raiseAmt <= callAmt) return "CALL";
                 { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << "AI Debug: Making semi-bluff raise." << std::endl; }
                 return "RAISE " + std::to_string(raiseAmt);
             }

             // Value Raise Logic
             if (equity > 0.85 && !strongDraw) { // Raise monster made hands
                 int raiseAmt = callAmt * 2 + pot; if (raiseAmt > ai.chips) raiseAmt = ai.chips; if (raiseAmt <= callAmt) return "CALL";
                 return "RAISE " + std::to_string(raiseAmt);
             }
             return "CALL"; // Good equity or good draw odds
        } else { // --- Not profitable ---
             { // Log the fold reason
                 std::lock_guard<std::mutex> lock(g_io_mutex);
                 std::cout << "AI Debug: Folding. Equity " << (equity * 100.0) << "% < Required " << (requiredEquity * 100.0) << "%." << std::endl;
             }
            return "FOLD";
        }
    }
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
        std::stringstream ss_cards; ss_cards << "CARDS";
        for(auto& c : communityCards) { ss_cards << " " << c.toString(); }
        broadcast(ss_cards.str());
    }
    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        std::cout << ss.str();
        if(communityCards.size()>0){ std::cout << "Community Cards:\n" << displayCards(communityCards); }
    }
}

// ===== Reset Function =====
void resetForNextHand() {
    std::lock_guard<std::mutex> lock(g_players_mutex);
    pot = 0; currentBet = 0; communityCards.clear(); g_preFlopRaiseMade = false; // Reset PFR tracker
    for (auto it = players.begin(); it != players.end(); ) {
        if (!it->isConnected || it->chips <= 0) { if (!it->isAI) std::cout << it->name << " has been removed." << std::endl; it = players.erase(it); }
        else { it->hand.clear(); it->folded = false; it->allIn = false; it->currentBet = 0; ++it; }
    }
    createDeck(); shuffleDeck();
}

// ===== Handle Incoming Messages =====
void handleIncomingMessage(int socket, const std::string& msg_data) {
    Player* p = getPlayerBySocket(socket); if (!p) return;
    if (msg_data == "DISCONNECTED") { p->isConnected = false; p->folded = true; broadcast(p->name + " disconnected."); }
    else if (msg_data.find("CHAT:") == 0) { broadcastChat(p->name, msg_data.substr(5)); }
}

// ===== Player Input =====
std::string getPlayerInput(Player &p) {
    sendToPlayer(p, "YOUR_MOVE");
    while (true) {
        Message msg; bool foundMessage = false;
        { std::lock_guard<std::mutex> lock(g_inbound_mutex); if (!g_inbound_messages.empty()) { msg = g_inbound_messages.front(); g_inbound_messages.pop(); foundMessage = true; } }
        if (foundMessage) { if (msg.socket == p.socket) { if (msg.data.find("CHAT:")==0) { handleIncomingMessage(msg.socket, msg.data); continue; } else if (msg.data=="DISCONNECTED") { handleIncomingMessage(msg.socket, msg.data); return "FOLD"; } return msg.data; } else { handleIncomingMessage(msg.socket, msg.data); } }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ===== Betting Round =====
void bettingRound(int roundNumber) {
    int raisesThisRound = 0; currentBet = 0;
    for (auto& p : players) p.currentBet = 0;
    int turnIndex = 0;
    bool actionOpened = false; // Track if anyone has bet/raised yet this round (for VPIP/PFR)

    while(true) {
        int activePlayers = 0; for(auto &p : players) if(!p.folded && !p.allIn && p.isConnected) activePlayers++;
        if(activePlayers <= 1) break;
        Player &p = players[turnIndex % players.size()];
        if(!p.folded && !p.allIn && p.isConnected) {
            showTable(); std::string action;
            if(p.isAI) { action = AIAction(p, roundNumber, deck); }
            else { action = getPlayerInput(p); }
            int callAmt = currentBet - p.currentBet; bool isVoluntaryAction = false; bool isRaiseAction = false;
            if(action.find("FOLD") != std::string::npos) { p.folded = true; broadcast(p.name + " folds."); }
            else if(action.find("CHECK") != std::string::npos) { if (callAmt==0) { broadcast(p.name+" checks."); } else { broadcast(p.name+" folded."); p.folded=true; } }
            else if(action.find("CALL") != std::string::npos) { if (callAmt==0) { broadcast(p.name+" checks."); } else { if (callAmt >= p.chips) { callAmt=p.chips; p.allIn=true; broadcast(p.name+" calls ALL-IN!"); } else { broadcast(p.name+" calls "+std::to_string(callAmt)+"."); } p.chips-=callAmt; pot+=callAmt; p.currentBet+=callAmt; isVoluntaryAction = true; } }
            else if(action.find("RAISE") != std::string::npos) { int raiseAmt=0; try { raiseAmt=std::stoi(action.substr(action.find(" ")+1)); } catch (...) { raiseAmt=50; } int totalBet=currentBet+raiseAmt; int amountToPutIn=totalBet-p.currentBet; if (amountToPutIn >= p.chips) { amountToPutIn=p.chips; totalBet=p.currentBet+amountToPutIn; p.allIn=true; broadcast(p.name+" raises ALL-IN!"); } else { broadcast(p.name+" raises "+std::to_string(raiseAmt)+"."); } p.chips-=amountToPutIn; pot+=amountToPutIn; p.currentBet=totalBet; currentBet=totalBet; raisesThisRound++; isVoluntaryAction=true; isRaiseAction=true; }
            else { broadcast(p.name+" folded."); p.folded=true; }

            // --- Update Opponent Model Stats (Pre-flop only) ---
            if (roundNumber == 0 && !p.isAI) {
                if (isVoluntaryAction) p.vpipActions++;
                if (isRaiseAction && !g_preFlopRaiseMade) { // Only count the *first* raise pre-flop
                    p.pfrActions++;
                    g_preFlopRaiseMade = true;
                }
            }
            if (isRaiseAction || (callAmt > 0 && action.find("CALL") != std::string::npos)) actionOpened = true; // Track if action has been opened
        }
        turnIndex++;
        bool roundFinished = true; int lastBet = -1; int activeInRound = 0;
        for(auto &pl : players) { if(pl.folded || !pl.isConnected) continue; if (!pl.allIn) { if (lastBet==-1) lastBet=pl.currentBet; if (pl.currentBet != lastBet) roundFinished=false; activeInRound++; } }
        if (activeInRound > 0 && roundFinished && turnIndex >= players.size()) break;
        if (activePlayers <= 1) break;
    }
    for(auto &p:players) p.currentBet = 0;
}

// ===== Client Handler =====
void clientHandler(int clientSocket) {
    char buffer[1024]; std::string networkBuffer = "";
    try { while(true) { int valread = read(clientSocket, buffer, 1023); if(valread <= 0) { std::lock_guard<std::mutex> lock(g_inbound_mutex); g_inbound_messages.push({clientSocket, "DISCONNECTED"}); break; } buffer[valread] = '\0'; networkBuffer += buffer; size_t pos; while ((pos = networkBuffer.find('\n')) != std::string::npos) { std::string message = networkBuffer.substr(0, pos); networkBuffer.erase(0, pos + 1); message.erase(std::remove(message.begin(), message.end(), '\r'), message.end()); std::lock_guard<std::mutex> lock(g_inbound_mutex); g_inbound_messages.push({clientSocket, message}); } } } catch (const std::exception& e) { std::lock_guard<std::mutex> lock(g_io_mutex); std::cerr << "Ex in clientHandler:" << e.what() << std::endl; } close(clientSocket);
}

// ===== Check if Hand Over =====
bool checkIfHandOver() {
    int activePlayers = 0; Player* winner = nullptr;
    { std::lock_guard<std::mutex> lock(g_players_mutex); for (auto& p : players) { if (!p.folded && p.isConnected) { activePlayers++; winner = &p; } } }
    if (activePlayers <= 1 && winner != nullptr) { std::string winMsg = winner->name + " wins " + std::to_string(pot) + " chips (last standing)!"; broadcast(winMsg); { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << winMsg << std::endl; } winner->chips += pot; return true; } return false;
}

// ===== Main =====
int main(){
    { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << "AI player? (y/n):"; char c; std::cin>>c; std::cin.ignore(); if(c=='y'||c=='Y'){ Player ai; ai.name="AI_Bot"; ai.isAI=true; players.push_back(ai); std::cout<<"AI joined.\n"; } }
    int server_fd; struct sockaddr_in address; int opt=1; socklen_t addrlen=sizeof(address); server_fd = socket(AF_INET, SOCK_STREAM, 0); setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)); address.sin_family=AF_INET; address.sin_addr.s_addr=INADDR_ANY; address.sin_port=htons(PORT); bind(server_fd,(struct sockaddr*)&address,sizeof(address)); listen(server_fd, MAX_PLAYERS);
    { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << "Server started on port " << PORT << ". Waiting...\n"; }
    std::thread([&](){ while(true) { int new_socket=accept(server_fd,(struct sockaddr*)&address,&addrlen); if(new_socket<0) continue; char b[1024]={0}; int vr=read(new_socket,b,1024); if(vr<=0){ close(new_socket); continue; } std::string pn(b,vr); pn.erase(std::remove(pn.begin(),pn.end(),'\n'),pn.end()); pn.erase(std::remove(pn.begin(),pn.end(),'\r'),pn.end()); { std::lock_guard<std::mutex> lock(g_players_mutex); if(players.size()>=MAX_PLAYERS){ send(new_socket,"SERVER_FULL\n",12,0); close(new_socket); continue; } Player p; p.name=pn; p.socket=new_socket; players.push_back(p); send(new_socket,("WELCOME "+pn+"\n").c_str(),pn.size()+9,0); std::cout<<pn<<" connected.\n"; } std::thread(clientHandler,new_socket).detach(); } }).detach();
    std::string command; while(true) { { std::lock_guard<std::mutex> lock(g_players_mutex); std::cout<<"\nPlayers("<<players.size()<<"/"<<MAX_PLAYERS<<"):"; for(auto &p:players)std::cout<<p.name<<" "; std::cout<<std::endl; } std::cout<<"Type 'start':"; std::getline(std::cin, command); if(command=="start"){ if(players.size()>=2)break; std::cout<<"Need >= 2 players.\n"; } }

    while(true) { // Main Game Loop
        resetForNextHand();
        if (players.size()<2) { std::cout<<"Not enough players.\n"; broadcast("Not enough players."); break; }

        // --- FIX: Correct Ante Collection Logic ---
        { // Use a block for the mutex lock
            std::lock_guard<std::mutex> lock(g_players_mutex); // LOCK 1
            std::stringstream ante_msg;
            ante_msg << "Collecting ante of " << ANTE_AMOUNT << " chips from each player.";
            {
                 std::lock_guard<std::mutex> io_lock(g_io_mutex); // LOCK 2 (nested) - OK
                 std::cout << ante_msg.str() << std::endl;
            } // LOCK 2 released
            broadcast_unsafe(ante_msg.str()); // FIX: Use unsafe version

            for (auto& p : players) {
                 if (p.isConnected) {
                    int ante = std::min(ANTE_AMOUNT, p.chips);
                    p.chips -= ante;
                    pot += ante;
                    if (p.chips == 0 && ante > 0) { // Check ante > 0
                        p.allIn = true;
                        broadcast_unsafe(p.name + " is all-in from the ante."); // FIX: Use unsafe here too
                    }
                }
            }
            // Announce the starting pot size
            std::stringstream pot_msg;
            pot_msg << "Pot starts at " << pot << " chips.";
             {
                 std::lock_guard<std::mutex> io_lock(g_io_mutex); // LOCK 2 (nested) - OK
                 std::cout << pot_msg.str() << std::endl;
            } // LOCK 2 released
            broadcast_unsafe(pot_msg.str()); // FIX: Use unsafe version
        } // LOCK 1 released
        // --- End Ante Collection ---


        // --- Update handsPlayed for opponent modeling ---
        { std::lock_guard<std::mutex> lock(g_players_mutex); for(auto& p : players) p.handsPlayed++; }

        broadcast("GAME_STARTING");
        for(auto &p : players) { if(p.hand.empty()) { p.hand.push_back(drawCard()); p.hand.push_back(drawCard()); if(!p.isAI) { sendToPlayer(p, "HOLE "+p.hand[0].toString()+" "+p.hand[1].toString()); } else { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout<<"AI hole cards:\n"<<displayCards(p.hand); } } }
        bettingRound(0); if (checkIfHandOver()) continue;
        for(int i=0;i<3;i++) communityCards.push_back(drawCard()); showTable(); bettingRound(1); if (checkIfHandOver()) continue;
        communityCards.push_back(drawCard()); showTable(); bettingRound(2); if (checkIfHandOver()) continue;
        communityCards.push_back(drawCard()); showTable(); bettingRound(3); if (checkIfHandOver()) continue;

        broadcast("\n--- SHOWDOWN ---"); { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << "\n--- SHOWDOWN ---" << std::endl; }
        Player* winner = nullptr; HandResult bestHand = {0, "Nothing"};
        { std::lock_guard<std::mutex> lock(g_players_mutex); for (auto& p : players) { if (!p.folded && p.isConnected) { std::string bcHand = p.name+"'s hand: "+p.hand[0].toString()+" "+p.hand[1].toString(); broadcast_unsafe(bcHand); std::string coutHand = p.name+"'s hand: "+p.hand[0].rank+p.hand[0].suit+" "+p.hand[1].rank+p.hand[1].suit; { std::lock_guard<std::mutex> io_lock(g_io_mutex); std::cout << coutHand << std::endl; } HandResult hand = getFullPlayerHand(p, communityCards); if (hand.rank > bestHand.rank) { bestHand = hand; winner = &p; } } } }
        if (winner != nullptr) { std::string winMsg = winner->name+" wins "+std::to_string(pot)+" chips with "+bestHand.name+"!"; broadcast(winMsg); { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << winMsg << std::endl; } winner->chips += pot; } else { std::string noWin = "No winner, pot split (NI)."; broadcast(noWin); { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << noWin << std::endl; } }

        std::string choice; { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout << "--- Hand Over ---\nAnother round? (y/n):"; }
        broadcast("HAND_OVER\nWaiting for admin..."); std::cin.clear(); std::getline(std::cin, choice);
        if (choice.empty()||(choice[0]!='y'&&choice[0]!='Y')) { broadcast("Game ending."); { std::lock_guard<std::mutex> lock(g_io_mutex); std::cout<<"Shutting down.\n"; } break; }
    }
    std::cout << "Game Over.\n"; return 0;
}