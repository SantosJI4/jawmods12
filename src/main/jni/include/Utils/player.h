vector<void*> players;

bool playerFind(void *player) {
    if (player != NULL) {
        for (int i = 0; i < players.size(); i++) {
            if (player == players[i]) return true;
        }
    }
    return false;
}
