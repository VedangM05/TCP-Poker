#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <atomic> // For myTurn
#include <cctype> // For ::isspace

#define PORT 5555

// --- ANSI Color Codes ---
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define RED     "\033[31m"      /* Red */
#define GREEN   "\033[32m"      /* Green */
#define YELLOW  "\033[33m"      /* Yellow */
#define BLUE    "\033[34m"      /* Blue */
#define MAGENTA "\033[35m"      /* Magenta */
#define CYAN    "\033[36m"      /* Cyan */
#define WHITE   "\033[37m"      /* White */
// --- End Color Codes ---

int g_sock = 0;
std::atomic<bool> g_myTurn{false};
std::vector<std::string> g_holeCards;
std::vector<std::string> g_communityCards;


// ===== REWRITTEN: Utility to Display Cards =====
// This version now translates ASCII letters back to Unicode symbols
// ===== REWRITTEN: Utility to Display Cards (with Colors) =====
std::string displayCards(const std::vector<std::string> &cards) {
    std::stringstream ss;
    for (int line = 0; line < 5; line++) {
        for (const auto &c_str : cards) {
            if (c_str.empty()) continue;

            std::string rank = c_str.substr(0, c_str.size() - 1);
            std::string suit_letter(1, c_str.back());
            std::string displaySuit = "?";
            std::string color = WHITE; // Default color

            if (suit_letter == "H") { displaySuit = "♥"; color = RED; }
            else if (suit_letter == "D") { displaySuit = "♦"; color = RED; }
            else if (suit_letter == "C") { displaySuit = "♣"; color = CYAN; } // Use Cyan instead of Black for visibility
            else if (suit_letter == "S") { displaySuit = "♠"; color = CYAN; }

            if (line == 0) ss << "┌─────┐ ";
            else if (line == 1) ss << "│" << color << rank << RESET << (rank.size() == 1 ? "    │ " : "   │ ");
            else if (line == 2) ss << "│  " << color << displaySuit << RESET << "  │ ";
            else if (line == 3) ss << "│" << (rank.size() == 1 ? "    " : "   ") << color << rank << RESET << "│ ";
            else ss << "└─────┘ ";
        }
        ss << "\n";
    }
    return ss.str();
}
// ===== REWRITTEN: Receive Thread (with Colors) =====
// ===== REWRITTEN: Receive Thread (with Colors) =====
void receiveMessages() {
    char buffer[1024];
    std::string networkBuffer = "";

    while(true) {
        int valread = read(g_sock, buffer, 1023);
        if(valread <= 0) {
            std::cout << RED << "Disconnected from server." << RESET << std::endl;
            g_myTurn = false;
            close(g_sock);
            exit(0);
        }
        buffer[valread] = '\0';
        networkBuffer += buffer;

        size_t pos;
        while ((pos = networkBuffer.find('\n')) != std::string::npos) {
            std::string msg = networkBuffer.substr(0, pos);
            networkBuffer.erase(0, pos + 1);

            // --- THIS IS THE CRITICAL FIX ---
            // It MUST be here, right after the message is extracted.
            msg.erase(std::remove(msg.begin(), msg.end(), '\r'), msg.end());
            // --- END CRITICAL FIX ---


            // --- Process the clean message with Colors ---

            if (msg.find("GAME_STARTING") != std::string::npos) {
                g_communityCards.clear();
                std::cout << "\n" << MAGENTA << "-------------------------------" << RESET << "\n";
                std::cout << BOLD << MAGENTA << "--- NEW HAND STARTING ---" << RESET << "\n" << std::endl;
            }
            else if (msg.find("YOUR_MOVE") != std::string::npos) {
                std::cout << "\n" << BOLD << CYAN << ">>> YOUR TURN TO ACT <<<" << RESET << std::endl;
                g_myTurn = true;
            }
            else if (msg.find("HOLE ") == 0) {
                g_holeCards.clear();
                std::string rest = msg.substr(5);
                std::stringstream ss_cards(rest);
                std::string card;
                while (ss_cards >> card) { g_holeCards.push_back(card); }
                std::cout << YELLOW << "--- Your Hole Cards ---" << RESET << std::endl;
                std::cout << displayCards(g_holeCards); // displayCards now handles colors
            }
            else if (msg.find("CARDS ") == 0) {
                g_communityCards.clear();
                std::string rest = msg.substr(6);
                std::stringstream ss_cards(rest);
                std::string card;
                while (ss_cards >> card) { g_communityCards.push_back(card); }

                std::string stageName = "";
                if (g_communityCards.size() == 3) stageName = "FLOP";
                else if (g_communityCards.size() == 4) stageName = "TURN";
                else if (g_communityCards.size() == 5) stageName = "RIVER";

                if (!stageName.empty()) {
                     std::cout << "\n" << MAGENTA << "--- " << stageName << " ---" << RESET;
                }

                std::cout << "\n" << YELLOW << "--- Your Hand ---" << RESET << std::endl;
                std::cout << displayCards(g_holeCards);
                std::cout << YELLOW << "--- Community Cards ---" << RESET << std::endl;
                std::cout << displayCards(g_communityCards) << std::endl;
            }
            else if (msg.find("CHAT:") == 0) {
                std::string chat = msg.substr(5);
                size_t colonPos = chat.find(':');
                if (colonPos != std::string::npos) {
                    std::string name = chat.substr(0, colonPos);
                    std::string chatMsg = chat.substr(colonPos + 1);
                    std::cout << "[" << YELLOW << name << RESET << "]: " << chatMsg << std::endl;
                }
            }
            // ===== MODIFIED: This block now parses cards =====
// ===== MODIFIED: Showdown hands as TEXT with COLOR SUITS =====
// ===== MODIFIED: Showdown hands as TEXT with COLOR SUITS =====
            else if (msg.find("'s hand: ") != std::string::npos) {
                // Parse the message e.g., "AI_Bot's hand: 7D 8S"
                size_t nameEndPos = msg.find("'s hand: ");
                std::string namePart = msg.substr(0, nameEndPos + 10); // "AI_Bot's hand: "
                std::string cardData = msg.substr(nameEndPos + 10);    // "7D 8S"

                // Print the name part in yellow
                std::cout << YELLOW << namePart << RESET;

                // Now parse and print the cards with color
                std::stringstream ss_cards(cardData);
                std::string card;
                while (ss_cards >> card) {
                    if (card.empty()) continue;

                    std::string rank = card.substr(0, card.size() - 1);
                    std::string suit_letter(1, card.back());
                    std::string displaySuit = "?";
                    std::string color = WHITE; // Default color

                    // --- THIS IS THE COLOR LOGIC ---
                    if (suit_letter == "H") { displaySuit = "♥"; color = RED; }
                    else if (suit_letter == "D") { displaySuit = "♦"; color = RED; }
                    else if (suit_letter == "C") { displaySuit = "♣"; color = CYAN; }
                    else if (suit_letter == "S") { displaySuit = "♠"; color = CYAN; }
                    // --- END COLOR LOGIC ---

                    // --- THIS IS THE FIX ---
                    // This prints the rank (e.g., "J") and the suit (e.g., "♦")
                    // in the SAME color (e.g., RED).
                    std::cout << color << rank << displaySuit << RESET << " ";
                }
                std::cout << std::endl; // End the line
            }
            // ===== END MODIFICATION =====            // ===== END MODIFICATION =====            // ===== END MODIFICATION =====
             else if (msg.find("Pot: ") != std::string::npos) {
                 // Color Pot line green
                 std::cout << GREEN << msg << RESET << std::endl;
             }
             else if (msg.find(" folds.") != std::string::npos) {
                 // Color fold action red
                 std::cout << RED << msg << RESET << std::endl;
             }
             else if (msg.find(" checks.") != std::string::npos || msg.find(" calls ") != std::string::npos) {
                 // Color check/call action yellow
                 std::cout << YELLOW << msg << RESET << std::endl;
             }
              else if (msg.find(" raises ") != std::string::npos) {
                 // Color raise action green
                 std::cout << GREEN << msg << RESET << std::endl;
             }
             else if (msg.find(" wins ") != std::string::npos || msg.find("Split pot!") != std::string::npos) {
                  // Color winner announcement green and bold
                 std::cout << BOLD << GREEN << msg << RESET << std::endl;
             }
             else if (msg.find("--- SHOWDOWN ---") != std::string::npos || msg.find("--- Hand Over ---") != std::string::npos) {
                  // Color delimiters magenta
                 std::cout << MAGENTA << msg << RESET << std::endl;
             }
            else {
                // Print all other server messages normally (white)
                std::cout << msg << std::endl;
            }
        }
    }
}

// ===== Main Function =====
int main() {
    std::string serverIP, playerName;
    std::cout << "Enter server IP (e.g., 127.0.0.1): ";
    std::getline(std::cin, serverIP);
    std::cout << "Enter your player name: ";
    std::getline(std::cin, playerName);

    struct sockaddr_in serv_addr;
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, serverIP.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cout << "Invalid address.\n"; return -1;
    }
    if (connect(g_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cout << "Connection failed.\n"; return -1;
    }

    send(g_sock, (playerName + "\n").c_str(), playerName.size() + 1, 0);

    std::thread recvThread(receiveMessages);
    recvThread.detach();

    std::cout << "Connected! Waiting for game to start..." << std::endl;
    std::cout << "Type '/chat <msg>' to chat." << std::endl;
    std::cout << "Type 'FOLD', 'CALL', 'CHECK', or 'RAISE <amount>' when it's your turn." << std::endl;

    std::string input;
    while(std::getline(std::cin, input)) {
        if(input.empty()) continue;

        if (input.find("/chat ") == 0) {
            std::string chatMsg = "CHAT:" + input.substr(6) + "\n";
            send(g_sock, chatMsg.c_str(), chatMsg.size(), 0);
        }
        else if (g_myTurn) {
            std::string upperInput = input;
            std::transform(upperInput.begin(), upperInput.end(), upperInput.begin(), ::toupper);

            if (upperInput.find("FOLD") != 0 && upperInput.find("CALL") != 0 &&
                upperInput.find("RAISE") != 0 && upperInput.find("CHECK") != 0) {
                std::cout << "Invalid command. Use FOLD, CALL, CHECK, or RAISE <amount>." << std::endl;
            } else {
                send(g_sock, (input + "\n").c_str(), input.size() + 1, 0);
                g_myTurn = false;
            }
        }
        else {
            std::cout << "It's not your turn to make a move." << std::endl;
        }
    }

    close(g_sock);
    return 0;
}

